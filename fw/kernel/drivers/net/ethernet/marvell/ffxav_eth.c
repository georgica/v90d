// SPDX-License-Identifier: GPL-2.0
/*
 * Ethernet of the Marvell FFxAV (88E7251), as on the Obihai OBi200: the
 * "UniMAC AV" DMA controller at 0x80008000 (interrupt 9), wired inside the
 * chip to port 5 of the embedded switch ("ESU", 0x8000e000), whose other
 * ports carry the LAN jack.
 *
 * Reconstructed from the board's own Linux 2.6.30 driver (uniAV_ and mvEth
 * functions, and the switch library, qd and gt functions); see fw/kernel/ETH-NOTES.md for the
 * register map and what is known and what is guessed.
 *
 * The controller has ten transmit and ten receive queues, each a ring of
 * 16-byte descriptors in memory, a ring base register and a producer index
 * register (software side); finished frames are counted in a status
 * register and acknowledged one by one. Descriptors carry no ownership bit.
 *
 * Every frame between the controller and the switch carries the 2-byte
 * Marvell header in front of the destination address: on transmit
 * DBNum << 12 | port vector (marvell,tx-header, 0x002f as the maker's
 * application has it), on receive DBNum << 12 | source port. Received
 * buffers hold header, frame and FCS.
 *
 * One network device; all receive queues feed it. NAPI.
 *
 * Written for the v90d project.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>

#define DRV_NAME	"ffxav_eth"

/* controller: transmit side at 0x000, receive side at 0x400 */
#define TX_GCFG		0x000	/* 0x80000c70 written at init */
#define TX_QMASK	0x004	/* 0x3ff written at init */
#define TX_DONE		0x008	/* [31:16] finished frames, [3:0] their queue */
#define TX_IND		0x00c	/* indirect access, see ffxav_ind_*() */
#define TXQ_CFG(q)	(0x080 + (q) * 0x20)	/* [10:8] ring size code */
#define TXQ_PROD(q)	(0x084 + (q) * 0x20)	/* producer index, 11 bits */
#define TXQ_ACK(q)	(0x088 + (q) * 0x20)	/* write 0x8001: one frame done */
#define TXQ_BASE(q)	(0x08c + (q) * 0x20)	/* ring bus address */
#define RX_GCFG		0x400
#define RX_QMASK	0x404
#define RX_DONE		0x408
#define RX_IND		0x40c
#define RXQ_CFG(q)	(0x480 + (q) * 0x20)	/* [31:16] buffer size too */
#define RXQ_PROD(q)	(0x484 + (q) * 0x20)
#define RXQ_ACK(q)	(0x488 + (q) * 0x20)
#define RXQ_BASE(q)	(0x48c + (q) * 0x20)
#define TX_COAL		0x800	/* [22:7] coalescing */
#define RX_MODE		0x804	/* [2:0] header mode, [22:7] coalescing */
#define INT_CAUSE	0x818	/* write back what was read to clear */
#define INT_MASK	0x81c
#define RX_CHAN		0x840	/* [9:0] queue enables, bit 31 busy */
#define TX_CHAN		0x844

#define GCFG_VAL	0x80000c70
#define QCFG_VAL	0x2160
#define QCFG_SIZE	GENMASK(10, 8)
#define QACK_ONE	0x8001
#define CHAN_BUSY	BIT(31)
#define IND_READ_Q	0x82100000	/* read indirect register 0x10 */
#define IND_WRITE_Q	0x83100000	/* write it, data in [15:0] */
#define HDR_MODE	1		/* Marvell header, as the maker's */
#define IDX_MASK	0x7ff

#define INT_TX		BIT(2)
#define INT_RX		BIT(3)
#define INT_RX_MORE	BIT(10)
#define INT_ALL		(INT_TX | INT_RX)

#define NUM_Q		10
#define ALL_Q		GENMASK(NUM_Q - 1, 0)

/* descriptor command and status bits */
#define D_FIRST		0x20
#define D_LAST		0x10
#define D_TX_IRQ	0x08	/* set with first and last by the maker */
#define D_RX_ERR	0x06	/* 4: bad FCS, 2 and 6: other errors */
#define D_RX_CRC	0x04

#define TX_RING		64
#define RX_RING0	64
#define RX_RING_N	16	/* queues 1 to 9 */
#define RX_BUF		1536	/* as programmed into RXQ_CFG */
#define RX_ALIGN	32
#define MV_HLEN		2	/* the Marvell header */

/* the switch: SMI device d, register r at (d << 7) | (r << 2) */
#define SW_PHY(p)	(p)
#define SW_PORT(p)	(8 + (p))
#define SW_GLOBAL1	15
#define SW_GLOBAL2	7
#define SW_NPORTS	7
#define SW_CPU_PORT	5
#define SW_ID		0xc000		/* port register 3, revision masked */

#define PORT_STATUS	0x00
#define PORT_ID		0x03
#define PORT_CTRL	0x04
#define PORT_VLAN	0x06
#define PORT_ERATE	0x09
#define PORT_ERATE2	0x0a
#define PORT_ETYPE	0x0f
#define G1_MONITOR	0x1a		/* [7:4] CPU destination */
#define G2_MGMT		0x05		/* bit 12 flood broadcast */

#define CTRL_STATE	GENMASK(1, 0)	/* 3 forwarding */
#define CTRL_FLOOD	GENMASK(3, 2)	/* 3 egress all unknown */
#define CTRL_FRAME	GENMASK(9, 8)	/* 3 EtherType DSA */
#define CTRL_HEADER	BIT(11)

#define EDSA_ETYPE	0x9000

struct ffxav_desc {
	__le16 len;
	__le16 cmd;
	__le32 info;
	__le32 buf;
	__le32 cookie;		/* software's in the maker's driver */
};

struct ffxav_rxq {
	struct ffxav_desc *ring;
	dma_addr_t ring_dma;
	unsigned int size;
	unsigned int cur;	/* next the hardware fills */
	unsigned int dirty;	/* next to give a buffer */
	struct sk_buff **skb;
	dma_addr_t *dma;
};

struct ffxav_priv {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *base;
	void __iomem *sw;
	int irq;
	struct napi_struct napi;
	struct work_struct reset_work;

	void *rings;
	dma_addr_t rings_dma;
	size_t rings_size;
	bool rings_sram;

	struct ffxav_desc *tx_ring;
	dma_addr_t tx_ring_dma;
	struct sk_buff *tx_skb[TX_RING];
	dma_addr_t tx_dma[TX_RING];
	unsigned int tx_len[TX_RING];
	unsigned int tx_head;	/* free running */
	unsigned int tx_tail;

	struct ffxav_rxq rxq[NUM_Q];
	u16 tx_header;
	u32 cpu_port;
};

static bool dump_regs = true;
module_param(dump_regs, bool, 0444);
MODULE_PARM_DESC(dump_regs, "Print the controller's and the switch's registers at probe");

static int switch_setup = 1;
module_param(switch_setup, int, 0444);
MODULE_PARM_DESC(switch_setup, "Program the switch (0: leave it as found, 1: forward all ports to the CPU port)");

static bool cpu_rate_limit = true;
module_param(cpu_rate_limit, bool, 0444);
MODULE_PARM_DESC(cpu_rate_limit, "Limit the switch's traffic to the CPU as the maker's application does (port 5 registers 9, 10)");

static bool sram_rings;
module_param(sram_rings, bool, 0444);
MODULE_PARM_DESC(sram_rings, "Put the descriptor rings into the on-chip SRAM (reg \"sram\") instead of DRAM");

static char *mac;
module_param(mac, charp, 0444);
MODULE_PARM_DESC(mac, "MAC address (xx:xx:xx:xx:xx:xx) when the device tree has none");

static unsigned int rx_ring_size(unsigned int q)
{
	return q ? RX_RING_N : RX_RING0;
}

/* 16 ... 2048 descriptors: 0 ... 7 */
static u32 ring_code(unsigned int n)
{
	return ilog2(n) - 4;
}

static u32 sw_read(struct ffxav_priv *p, unsigned int dev, unsigned int reg)
{
	return readl(p->sw + (dev << 7) + (reg << 2)) & 0xffff;
}

static void sw_write(struct ffxav_priv *p, unsigned int dev, unsigned int reg,
		     u16 val)
{
	writel(val, p->sw + (dev << 7) + (reg << 2));
}

static void sw_rmw(struct ffxav_priv *p, unsigned int dev, unsigned int reg,
		   u16 mask, u16 val)
{
	sw_write(p, dev, reg, (sw_read(p, dev, reg) & ~mask) | (val & mask));
}

static int ffxav_wait(struct ffxav_priv *p, unsigned int reg)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout_atomic(p->base + reg, v, !(v & CHAN_BUSY),
					1, 10000);
	if (ret)
		dev_err(p->dev, "register 0x%03x stays busy (0x%08x)\n", reg, v);
	return ret;
}

/* the queue enables behind the indirect register 0x10 of a channel */
static int ffxav_ind_read(struct ffxav_priv *p, unsigned int ind, u32 *val)
{
	int ret;

	ret = ffxav_wait(p, ind);
	if (ret)
		return ret;
	writel(IND_READ_Q, p->base + ind);
	ret = ffxav_wait(p, ind);
	*val = readl(p->base + ind) & 0xffff;
	return ret;
}

static int ffxav_ind_write(struct ffxav_priv *p, unsigned int ind, u32 val)
{
	writel(IND_WRITE_Q | (val & 0xffff), p->base + ind);
	return ffxav_wait(p, ind);
}

/* channel 0 is transmit, 1 receive; mask has a bit per queue */
static void ffxav_chan_enable(struct ffxav_priv *p, bool rx, u32 mask)
{
	unsigned int ind = rx ? RX_IND : TX_IND;
	unsigned int en = rx ? RX_CHAN : TX_CHAN;
	u32 v;

	if (ffxav_ind_read(p, ind, &v))
		return;
	if (ffxav_ind_write(p, ind, v | mask))
		return;
	if (ffxav_wait(p, en))
		return;
	v = readl(p->base + en) & ALL_Q;
	writel(CHAN_BUSY | mask | v, p->base + en);
	ffxav_wait(p, en);
}

static void ffxav_chan_disable(struct ffxav_priv *p, bool rx, u32 mask)
{
	unsigned int ind = rx ? RX_IND : TX_IND;
	unsigned int en = rx ? RX_CHAN : TX_CHAN;
	u32 v;

	if (!ffxav_wait(p, en)) {
		v = readl(p->base + en);
		writel((v & ~mask) | CHAN_BUSY, p->base + en);
		ffxav_wait(p, en);
	}
	if (!ffxav_ind_read(p, ind, &v))
		ffxav_ind_write(p, ind, v & ~mask);
}

/* everything stopped, rings forgotten, no interrupts */
static void ffxav_hw_stop(struct ffxav_priv *p)
{
	unsigned int q;

	writel(0, p->base + INT_MASK);
	writel(readl(p->base + INT_CAUSE), p->base + INT_CAUSE);
	ffxav_chan_disable(p, false, ALL_Q);
	ffxav_chan_disable(p, true, ALL_Q);
	for (q = 0; q < NUM_Q; q++) {
		writel(0, p->base + TXQ_BASE(q));
		writel(0, p->base + TXQ_PROD(q));
		writel(0, p->base + RXQ_BASE(q));
		writel(0, p->base + RXQ_PROD(q));
	}
}

/* what mvEthInit() writes after its reset */
static void ffxav_hw_init(struct ffxav_priv *p)
{
	unsigned int q;
	u32 v;

	v = readl(p->base + TX_COAL);
	writel((v & GENMASK(22, 7)) | BIT(17), p->base + TX_COAL);
	v = readl(p->base + RX_MODE);
	writel((v & GENMASK(22, 7)) | HDR_MODE, p->base + RX_MODE);
	writel(GCFG_VAL, p->base + RX_GCFG);
	writel(GCFG_VAL, p->base + TX_GCFG);
	writel(ALL_Q, p->base + RX_QMASK);
	writel(ALL_Q, p->base + TX_QMASK);
	for (q = 0; q < NUM_Q; q++) {
		writel(RX_BUF << 16 | QCFG_VAL, p->base + RXQ_CFG(q));
		writel(QCFG_VAL, p->base + TXQ_CFG(q));
	}
}

static void ffxav_queue_start(struct ffxav_priv *p, bool rx, unsigned int q,
			      dma_addr_t ring, unsigned int size)
{
	unsigned int cfg = rx ? RXQ_CFG(q) : TXQ_CFG(q);
	u32 v;

	writel(ring, p->base + (rx ? RXQ_BASE(q) : TXQ_BASE(q)));
	v = readl(p->base + cfg) & ~QCFG_SIZE;
	writel(v, p->base + cfg);
	writel(v | FIELD_PREP(QCFG_SIZE, ring_code(size)), p->base + cfg);
	ffxav_chan_enable(p, rx, BIT(q));
}

/* give the hardware the buffer of slot rxq->dirty */
static void ffxav_rx_give(struct ffxav_priv *p, unsigned int q,
			  struct sk_buff *skb, dma_addr_t dma)
{
	struct ffxav_rxq *rxq = &p->rxq[q];
	unsigned int i = rxq->dirty;

	rxq->skb[i] = skb;
	rxq->dma[i] = dma;
	rxq->ring[i].buf = cpu_to_le32(dma);
	rxq->ring[i].cookie = 0;
	rxq->dirty = (i + 1) & (rxq->size - 1);
	/* writel orders the descriptor before the index */
	writel(rxq->dirty, p->base + RXQ_PROD(q));
}

static struct sk_buff *ffxav_rx_alloc(struct ffxav_priv *p, dma_addr_t *dma)
{
	struct sk_buff *skb;
	unsigned int off;

	skb = netdev_alloc_skb(p->ndev, RX_BUF + RX_ALIGN);
	if (!skb)
		return NULL;
	off = PTR_ALIGN(skb->data, RX_ALIGN) - skb->data;
	skb_reserve(skb, off);
	*dma = dma_map_single(p->dev, skb->data, RX_BUF, DMA_FROM_DEVICE);
	if (dma_mapping_error(p->dev, *dma)) {
		dev_kfree_skb_any(skb);
		return NULL;
	}
	return skb;
}

static void ffxav_free_bufs(struct ffxav_priv *p)
{
	unsigned int q, i;

	for (q = 0; q < NUM_Q; q++) {
		struct ffxav_rxq *rxq = &p->rxq[q];

		for (i = 0; i < rxq->size; i++) {
			if (!rxq->skb[i])
				continue;
			dma_unmap_single(p->dev, rxq->dma[i], RX_BUF,
					 DMA_FROM_DEVICE);
			dev_kfree_skb_any(rxq->skb[i]);
			rxq->skb[i] = NULL;
		}
		memset(rxq->ring, 0, rxq->size * sizeof(*rxq->ring));
		rxq->cur = 0;
		rxq->dirty = 0;
	}
	for (i = 0; i < TX_RING; i++) {
		if (!p->tx_skb[i])
			continue;
		dma_unmap_single(p->dev, p->tx_dma[i], p->tx_len[i],
				 DMA_TO_DEVICE);
		dev_kfree_skb_any(p->tx_skb[i]);
		p->tx_skb[i] = NULL;
	}
	memset(p->tx_ring, 0, TX_RING * sizeof(*p->tx_ring));
	p->tx_head = 0;
	p->tx_tail = 0;
}

/* a ring holds one buffer less than its size, as the maker's does */
static int ffxav_fill_rx(struct ffxav_priv *p)
{
	unsigned int q, i;

	for (q = 0; q < NUM_Q; q++) {
		for (i = 0; i < p->rxq[q].size - 1; i++) {
			struct sk_buff *skb;
			dma_addr_t dma;

			skb = ffxav_rx_alloc(p, &dma);
			if (!skb)
				return -ENOMEM;
			ffxav_rx_give(p, q, skb, dma);
		}
	}
	return 0;
}

static int ffxav_hw_up(struct ffxav_priv *p)
{
	unsigned int q;
	int ret;

	ffxav_hw_stop(p);
	ffxav_hw_init(p);
	ret = ffxav_fill_rx(p);
	if (ret)
		return ret;
	writel(readl(p->base + INT_CAUSE), p->base + INT_CAUSE);
	ffxav_queue_start(p, false, 0, p->tx_ring_dma, TX_RING);
	for (q = 0; q < NUM_Q; q++)
		ffxav_queue_start(p, true, q, p->rxq[q].ring_dma,
				  p->rxq[q].size);
	return 0;
}

static unsigned int ffxav_tx_free(struct ffxav_priv *p)
{
	return TX_RING - 1 - (p->tx_head - p->tx_tail);
}

static void ffxav_tx_clean(struct ffxav_priv *p, int budget)
{
	struct net_device *ndev = p->ndev;
	unsigned int n = 0, pkts = 0, bytes = 0;

	while (n++ < 2 * TX_RING) {
		u32 st = readl(p->base + TX_DONE);
		unsigned int q = st & 0xf, i;
		struct sk_buff *skb;

		if (!(st >> 16))
			break;
		if (q >= NUM_Q) {
			netdev_warn_once(ndev, "transmit status 0x%08x\n", st);
			break;
		}
		writel(QACK_ONE, p->base + TXQ_ACK(q));
		if (q != 0 || p->tx_tail == p->tx_head) {
			netdev_warn_once(ndev, "stray transmit completion 0x%08x\n", st);
			continue;
		}
		i = p->tx_tail % TX_RING;
		skb = p->tx_skb[i];
		dma_unmap_single(p->dev, p->tx_dma[i], p->tx_len[i],
				 DMA_TO_DEVICE);
		bytes += p->tx_len[i] - MV_HLEN;
		pkts++;
		napi_consume_skb(skb, budget);
		p->tx_skb[i] = NULL;
		p->tx_tail++;
	}
	if (!pkts)
		return;
	ndev->stats.tx_packets += pkts;
	ndev->stats.tx_bytes += bytes;
	/* pairs with the barrier in ffxav_xmit() */
	smp_mb();
	if (netif_queue_stopped(ndev) && ffxav_tx_free(p) > TX_RING / 4)
		netif_wake_queue(ndev);
}

/* a frame tagged by the switch (EtherType DSA): drop the 8 bytes of tag */
static void ffxav_strip_edsa(struct sk_buff *skb)
{
	if (skb->len < 2 * ETH_ALEN + 8 + 2 ||
	    get_unaligned_be16(skb->data + 2 * ETH_ALEN) != EDSA_ETYPE)
		return;
	memmove(skb->data + 8, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, 8);
}

static int ffxav_rx(struct ffxav_priv *p, int budget)
{
	struct net_device *ndev = p->ndev;
	int done = 0;

	while (done < budget) {
		u32 st = readl(p->base + RX_DONE);
		unsigned int q = st & 0xf, i, len, cmd;
		struct ffxav_rxq *rxq;
		struct sk_buff *skb, *nskb;
		dma_addr_t dma, ndma;

		if (!(st >> 16))
			break;
		if (q >= NUM_Q) {
			netdev_warn_once(ndev, "receive status 0x%08x\n", st);
			break;
		}
		rxq = &p->rxq[q];
		i = rxq->cur;
		len = le16_to_cpu(rxq->ring[i].len);
		cmd = le16_to_cpu(rxq->ring[i].cmd);
		writel(QACK_ONE, p->base + RXQ_ACK(q));
		rxq->cur = (i + 1) & (rxq->size - 1);
		skb = rxq->skb[i];
		dma = rxq->dma[i];
		rxq->skb[i] = NULL;
		done++;

		if (!skb) {
			/* nothing there: the ring and the hardware disagree */
			netdev_err_once(ndev, "receive queue %u slot %u empty (0x%04x)\n",
					q, i, cmd);
			ndev->stats.rx_errors++;
			nskb = ffxav_rx_alloc(p, &ndma);
			if (nskb)
				ffxav_rx_give(p, q, nskb, ndma);
			continue;
		}
		if ((cmd & (D_FIRST | D_LAST)) != (D_FIRST | D_LAST) ||
		    (cmd & D_RX_ERR) || len < MV_HLEN + ETH_HLEN + ETH_FCS_LEN ||
		    len > RX_BUF) {
			ndev->stats.rx_errors++;
			if ((cmd & D_RX_ERR) == D_RX_CRC)
				ndev->stats.rx_crc_errors++;
			else if (len > RX_BUF || len < MV_HLEN + ETH_HLEN)
				ndev->stats.rx_length_errors++;
			ffxav_rx_give(p, q, skb, dma);
			continue;
		}
		nskb = ffxav_rx_alloc(p, &ndma);
		if (!nskb) {
			ndev->stats.rx_dropped++;
			ffxav_rx_give(p, q, skb, dma);
			continue;
		}
		dma_unmap_single(p->dev, dma, RX_BUF, DMA_FROM_DEVICE);
		ffxav_rx_give(p, q, nskb, ndma);

		skb_put(skb, len - ETH_FCS_LEN);
		skb_pull(skb, MV_HLEN);
		ffxav_strip_edsa(skb);
		skb->protocol = eth_type_trans(skb, ndev);
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += skb->len + ETH_HLEN;
		napi_gro_receive(&p->napi, skb);
	}
	return done;
}

static int ffxav_poll(struct napi_struct *napi, int budget)
{
	struct ffxav_priv *p = container_of(napi, struct ffxav_priv, napi);
	int done;

	ffxav_tx_clean(p, budget);
	done = ffxav_rx(p, budget);
	if (done < budget && napi_complete_done(napi, done))
		writel(INT_ALL, p->base + INT_MASK);
	return done;
}

static irqreturn_t ffxav_irq(int irq, void *dev_id)
{
	struct ffxav_priv *p = netdev_priv(dev_id);
	u32 cause = readl(p->base + INT_CAUSE);

	if (!cause)
		return IRQ_NONE;
	/* the poll unmasks again; INT_RX_MORE is never unmasked here */
	writel(0, p->base + INT_MASK);
	writel(cause, p->base + INT_CAUSE);
	napi_schedule(&p->napi);
	return IRQ_HANDLED;
}

static netdev_tx_t ffxav_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct ffxav_priv *p = netdev_priv(ndev);
	struct ffxav_desc *d;
	unsigned int i;
	dma_addr_t dma;

	if (skb_put_padto(skb, ETH_ZLEN))
		goto dropped;
	if (skb_cow_head(skb, MV_HLEN))
		goto drop;
	skb_push(skb, MV_HLEN);
	put_unaligned_be16(p->tx_header, skb->data);

	dma = dma_map_single(p->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(p->dev, dma))
		goto drop;

	i = p->tx_head % TX_RING;
	p->tx_skb[i] = skb;
	p->tx_dma[i] = dma;
	p->tx_len[i] = skb->len;
	d = &p->tx_ring[i];
	d->buf = cpu_to_le32(dma);
	d->info = 0;
	d->cookie = 0;
	d->len = cpu_to_le16(skb->len);
	d->cmd = cpu_to_le16(D_FIRST | D_LAST | D_TX_IRQ);
	p->tx_head++;
	skb_tx_timestamp(skb);
	/* writel orders the descriptor before the index */
	writel(p->tx_head % TX_RING, p->base + TXQ_PROD(0));

	if (ffxav_tx_free(p) == 0) {
		netif_stop_queue(ndev);
		/* pairs with the barrier in ffxav_tx_clean() */
		smp_mb();
		if (ffxav_tx_free(p))
			netif_start_queue(ndev);
	}
	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
dropped:
	ndev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static int ffxav_open(struct net_device *ndev)
{
	struct ffxav_priv *p = netdev_priv(ndev);
	int ret;

	ret = ffxav_hw_up(p);
	if (ret) {
		ffxav_hw_stop(p);
		ffxav_free_bufs(p);
		return ret;
	}
	napi_enable(&p->napi);
	writel(INT_ALL, p->base + INT_MASK);
	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int ffxav_stop(struct net_device *ndev)
{
	struct ffxav_priv *p = netdev_priv(ndev);

	netif_stop_queue(ndev);
	writel(0, p->base + INT_MASK);
	napi_disable(&p->napi);
	netif_carrier_off(ndev);
	ffxav_hw_stop(p);
	ffxav_free_bufs(p);
	return 0;
}

static void ffxav_reset_work(struct work_struct *work)
{
	struct ffxav_priv *p = container_of(work, struct ffxav_priv, reset_work);

	rtnl_lock();
	if (netif_running(p->ndev)) {
		ffxav_stop(p->ndev);
		if (ffxav_open(p->ndev))
			netdev_err(p->ndev, "cannot restart after a transmit timeout\n");
	}
	rtnl_unlock();
}

static void ffxav_tx_timeout(struct net_device *ndev, unsigned int txq)
{
	struct ffxav_priv *p = netdev_priv(ndev);

	netdev_warn(ndev, "transmit timeout: head %u tail %u done 0x%08x cause 0x%08x\n",
		    p->tx_head, p->tx_tail, readl(p->base + TX_DONE),
		    readl(p->base + INT_CAUSE));
	ndev->stats.tx_errors++;
	schedule_work(&p->reset_work);
}

static const struct net_device_ops ffxav_netdev_ops = {
	.ndo_open		= ffxav_open,
	.ndo_stop		= ffxav_stop,
	.ndo_start_xmit		= ffxav_xmit,
	.ndo_tx_timeout		= ffxav_tx_timeout,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static void ffxav_get_drvinfo(struct net_device *ndev,
			      struct ethtool_drvinfo *info)
{
	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
	strscpy(info->bus_info, "platform", sizeof(info->bus_info));
}

static const struct ethtool_ops ffxav_ethtool_ops = {
	.get_drvinfo	= ffxav_get_drvinfo,
	.get_link	= ethtool_op_get_link,
};

/*
 * The switch as the board's kernel leaves it (mv_sw_init and the maker's
 * swcfg commands), less its AVB, QoS and address table work: all ports
 * forwarding, the CPU port with the Marvell header and flooding everything
 * unknown to the CPU (the controller has no address filter).
 */
static void ffxav_switch_setup(struct ffxav_priv *p)
{
	unsigned int port, cpu = p->cpu_port;
	u32 id = sw_read(p, SW_PORT(0), PORT_ID);

	if ((id & 0xfff0) != SW_ID) {
		dev_warn(p->dev, "switch ID 0x%04x, not 0x%04x: left alone\n",
			 id, SW_ID);
		return;
	}
	sw_rmw(p, SW_GLOBAL1, G1_MONITOR, GENMASK(7, 4), cpu << 4);
	sw_rmw(p, SW_GLOBAL2, G2_MGMT, BIT(12), BIT(12));
	sw_rmw(p, SW_PORT(cpu), PORT_CTRL,
	       CTRL_HEADER | CTRL_FRAME | CTRL_FLOOD,
	       CTRL_HEADER | FIELD_PREP(CTRL_FRAME, 3) |
	       FIELD_PREP(CTRL_FLOOD, 3));
	sw_write(p, SW_PORT(cpu), PORT_ETYPE, EDSA_ETYPE);
	if (cpu_rate_limit) {
		sw_write(p, SW_PORT(cpu), PORT_ERATE, 0x0028);
		sw_write(p, SW_PORT(cpu), PORT_ERATE2, 0xb0c8);
	}
	for (port = 0; port < SW_NPORTS; port++)
		sw_rmw(p, SW_PORT(port), PORT_CTRL, CTRL_STATE, 3);
}

static void ffxav_dump(struct ffxav_priv *p)
{
	static const u16 mac_regs[] = {
		TX_GCFG, TX_QMASK, TX_DONE, TX_IND, 0x010,
		0x080, 0x084, 0x08c, 0x090, 0x094, 0x098, 0x09c,
		RX_GCFG, RX_QMASK, RX_DONE, RX_IND, 0x410,
		0x480, 0x484, 0x48c, 0x490, 0x494, 0x498, 0x49c,
		TX_COAL, RX_MODE, 0x808, 0x80c, 0x810, 0x814,
		INT_CAUSE, INT_MASK, RX_CHAN, TX_CHAN,
	};
	static const u8 port_regs[] = {
		PORT_STATUS, 0x01, PORT_ID, PORT_CTRL, PORT_VLAN, 0x07,
		0x08, PORT_ERATE, PORT_ERATE2, PORT_ETYPE,
	};
	unsigned int i, port;
	u32 txq, rxq;

	for (i = 0; i < ARRAY_SIZE(mac_regs); i++)
		dev_info(p->dev, "mac %03x: %08x\n", mac_regs[i],
			 readl(p->base + mac_regs[i]));
	if (!ffxav_ind_read(p, TX_IND, &txq) && !ffxav_ind_read(p, RX_IND, &rxq))
		dev_info(p->dev, "queue enables: tx %04x rx %04x\n", txq, rxq);
	if (!p->sw)
		return;
	for (port = 0; port < SW_NPORTS; port++) {
		char line[96];
		int n = 0;

		for (i = 0; i < ARRAY_SIZE(port_regs); i++)
			n += scnprintf(line + n, sizeof(line) - n, " %x:%04x",
				       port_regs[i],
				       sw_read(p, SW_PORT(port), port_regs[i]));
		dev_info(p->dev, "switch port %u:%s\n", port, line);
		dev_info(p->dev, "switch phy %u: bmcr %04x bmsr %04x id %04x %04x lpa %04x\n",
			 port, sw_read(p, SW_PHY(port), 0),
			 sw_read(p, SW_PHY(port), 1),
			 sw_read(p, SW_PHY(port), 2),
			 sw_read(p, SW_PHY(port), 3),
			 sw_read(p, SW_PHY(port), 5));
	}
	dev_info(p->dev, "switch global1 0:%04x 4:%04x 1a:%04x global2 5:%04x\n",
		 sw_read(p, SW_GLOBAL1, 0), sw_read(p, SW_GLOBAL1, 4),
		 sw_read(p, SW_GLOBAL1, G1_MONITOR),
		 sw_read(p, SW_GLOBAL2, G2_MGMT));
}

static int ffxav_alloc_rings(struct platform_device *pdev,
			     struct ffxav_priv *p)
{
	struct resource *res;
	unsigned int q, off;
	size_t size = TX_RING;

	for (q = 0; q < NUM_Q; q++)
		size += rx_ring_size(q);
	size *= sizeof(struct ffxav_desc);
	p->rings_size = size;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (sram_rings && res && resource_size(res) >= size) {
		p->rings = devm_memremap(&pdev->dev, res->start, size,
					 MEMREMAP_WC);
		if (IS_ERR(p->rings))
			return PTR_ERR(p->rings);
		p->rings_dma = res->start;
		p->rings_sram = true;
		memset(p->rings, 0, size);
	} else {
		if (sram_rings)
			dev_warn(&pdev->dev, "no SRAM for the rings, in DRAM\n");
		p->rings = dmam_alloc_coherent(&pdev->dev, size, &p->rings_dma,
					       GFP_KERNEL);
		if (!p->rings)
			return -ENOMEM;
	}

	p->tx_ring = p->rings;
	p->tx_ring_dma = p->rings_dma;
	off = TX_RING * sizeof(struct ffxav_desc);
	for (q = 0; q < NUM_Q; q++) {
		struct ffxav_rxq *rxq = &p->rxq[q];

		rxq->size = rx_ring_size(q);
		rxq->ring = p->rings + off;
		rxq->ring_dma = p->rings_dma + off;
		off += rxq->size * sizeof(struct ffxav_desc);
		rxq->skb = devm_kcalloc(&pdev->dev, rxq->size,
					sizeof(*rxq->skb), GFP_KERNEL);
		rxq->dma = devm_kcalloc(&pdev->dev, rxq->size,
					sizeof(*rxq->dma), GFP_KERNEL);
		if (!rxq->skb || !rxq->dma)
			return -ENOMEM;
	}
	return 0;
}

static void ffxav_set_mac(struct platform_device *pdev, struct net_device *ndev)
{
	u8 addr[ETH_ALEN];

	if (!of_get_ethdev_address(pdev->dev.of_node, ndev))
		return;
	if (mac && mac_pton(mac, addr) && is_valid_ether_addr(addr)) {
		eth_hw_addr_set(ndev, addr);
		return;
	}
	eth_hw_addr_random(ndev);
	dev_warn(&pdev->dev, "no MAC address given, using %pM\n",
		 ndev->dev_addr);
}

static int ffxav_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *ndev;
	struct ffxav_priv *p;
	struct resource *res, *mres;
	u32 val;
	int ret;

	ndev = devm_alloc_etherdev(dev, sizeof(*p));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	p = netdev_priv(ndev);
	p->ndev = ndev;
	p->dev = dev;
	platform_set_drvdata(pdev, ndev);
	INIT_WORK(&p->reset_work, ffxav_reset_work);

	mres = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	p->base = devm_ioremap_resource(dev, mres);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "switch");
	if (res) {
		p->sw = devm_ioremap_resource(dev, res);
		if (IS_ERR(p->sw))
			return PTR_ERR(p->sw);
	}
	p->irq = platform_get_irq(pdev, 0);
	if (p->irq < 0)
		return p->irq;

	p->tx_header = 0x002f;
	if (!of_property_read_u32(dev->of_node, "marvell,tx-header", &val))
		p->tx_header = val;
	p->cpu_port = SW_CPU_PORT;
	of_property_read_u32(dev->of_node, "marvell,cpu-port", &p->cpu_port);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	if (dump_regs)
		ffxav_dump(p);

	/* the board's kernel may have left it running: stop it first */
	ffxav_hw_stop(p);

	ret = ffxav_alloc_rings(pdev, p);
	if (ret)
		return ret;

	if (p->sw && switch_setup)
		ffxav_switch_setup(p);

	ret = devm_request_irq(dev, p->irq, ffxav_irq, 0, DRV_NAME, ndev);
	if (ret)
		return ret;

	ndev->netdev_ops = &ffxav_netdev_ops;
	ndev->ethtool_ops = &ffxav_ethtool_ops;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->max_mtu = ETH_DATA_LEN;
	netif_napi_add(ndev, &p->napi, ffxav_poll);
	ffxav_set_mac(pdev, ndev);
	netif_carrier_off(ndev);

	ret = register_netdev(ndev);
	if (ret) {
		netif_napi_del(&p->napi);
		return ret;
	}
	netdev_info(ndev, "FFxAV Ethernet at %pa irq %d, rings in %s, tx header %04x, %pM\n",
		    &mres->start,
		    p->irq, p->rings_sram ? "SRAM" : "DRAM", p->tx_header,
		    ndev->dev_addr);
	return 0;
}

static int ffxav_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct ffxav_priv *p = netdev_priv(ndev);

	unregister_netdev(ndev);
	cancel_work_sync(&p->reset_work);
	netif_napi_del(&p->napi);
	ffxav_hw_stop(p);
	return 0;
}

static void ffxav_shutdown(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);

	ffxav_hw_stop(netdev_priv(ndev));
}

static const struct of_device_id ffxav_of_match[] = {
	{ .compatible = "marvell,ffxav-eth" },
	{ }
};
MODULE_DEVICE_TABLE(of, ffxav_of_match);

static struct platform_driver ffxav_driver = {
	.probe		= ffxav_probe,
	.remove		= ffxav_remove,
	.shutdown	= ffxav_shutdown,
	.driver		= {
		.name		= DRV_NAME,
		.of_match_table	= ffxav_of_match,
	},
};
module_platform_driver(ffxav_driver);

MODULE_DESCRIPTION("Marvell FFxAV (88E7251) Ethernet");
MODULE_LICENSE("GPL");
