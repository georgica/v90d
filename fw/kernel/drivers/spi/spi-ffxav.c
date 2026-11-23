// SPDX-License-Identifier: GPL-2.0
/*
 * SPI controller of the Marvell FFxAV (88E7251), as on the Obihai OBi200,
 * at 0x8000c800. It is a DesignWare APB SSI (the register layout, the
 * transfer modes, the data register at 0x60); the board's own driver
 * (ffxav_spi_*) uses it like this, and so do we:
 *
 *   0x00  CTRLR0  bits 3:0 frame size - 1, 6 CPHA, 7 CPOL, 9:8 mode
 *                 (0 transmit and receive, 1 transmit only, 3 "EEPROM
 *                 read": send what is queued, then clock in NDF + 1 frames)
 *   0x04  CTRLR1  NDF, frames to read - 1 in EEPROM read mode
 *   0x08  SSIENR  1 enabled; registers other than SER and DR are written
 *                 with it 0
 *   0x10  SER     a bit per chip select (the flash bit 0, the SLIC bit 1)
 *   0x14  BAUDR   divider of the 100 MHz clock, even, at least 2
 *   0x20  TXFLR   entries in the transmit FIFO (16 deep)
 *   0x24  RXFLR   entries in the receive FIFO
 *   0x28  SR      bit 0 busy, 1 transmit FIFO not full, 2 transmit FIFO
 *                 empty, 3 receive FIFO not empty
 *   0x2c  IMR     0: no interrupts, everything is polled
 *   0x60  DR      the FIFOs
 *
 * The chip select is the controller's own: it is asserted while a SER bit
 * is set and the transmit FIFO has data, and released as soon as the FIFO
 * runs dry. So a group of transfers that must share one chip select (a
 * flash command and its data) has to go out as one stream that never lets
 * the FIFO empty: we fill the FIFO with SER clear, set SER, and keep it fed
 * with interrupts off. A command followed by a read uses the EEPROM read
 * mode, as the board's driver does (and the board's m25p80 reads in pieces
 * of 32 bytes for that reason; we keep interrupts off instead, and limit a
 * transfer to max_xfer bytes, 256 by default, so that is never long).
 * Between two such groups the chip select is always released.
 *
 * Written for the v90d project from the board's own driver
 * (SPI-LED-WDT-NOTES.md); no vendor code.
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define SSI_CTRLR0	0x00
#define SSI_CTRLR1	0x04
#define SSI_SSIENR	0x08
#define SSI_SER		0x10
#define SSI_BAUDR	0x14
#define SSI_TXFTLR	0x18
#define SSI_RXFTLR	0x1c
#define SSI_TXFLR	0x20
#define SSI_RXFLR	0x24
#define SSI_SR		0x28
#define SSI_IMR		0x2c
#define SSI_RISR	0x34
#define SSI_ICR		0x48
#define SSI_IDR		0x58
#define SSI_VERSION	0x5c
#define SSI_DR		0x60

#define CR0_CPHA	BIT(6)
#define CR0_CPOL	BIT(7)
#define CR0_TMOD_SHIFT	8
#define TMOD_TR		0	/* transmit and receive */
#define TMOD_TO		1	/* transmit only */
#define TMOD_EEPROM	3	/* transmit, then receive NDF + 1 frames */
#define NDF_MAX		0x10000

#define SR_BUSY		BIT(0)
#define SR_TFE		BIT(2)
#define RISR_RXOI	BIT(3)

#define FFXAV_SPI_FIFO	16	/* the board's driver: 16 - TXFLR is room */
#define FFXAV_SPI_CLK	100000000
#define FFXAV_SPI_NCS	4

/*
 * The system register at 0x8000f0ac: the board's driver clears bit 27 and
 * sets bit 26, then waits 125 us, each time a device is set up. What the
 * bits do is not known (a reset or a clock of the SPI block or of the
 * SLIC, or the pins); we do the same once, at probe.
 */
#define SYS_SPI_CLR	BIT(27)
#define SYS_SPI_SET	BIT(26)

static unsigned int max_xfer = 256;
module_param(max_xfer, uint, 0644);
MODULE_PARM_DESC(max_xfer, "largest transfer (bytes) offered to SPI memory drivers");

struct ffxav_spi {
	struct device *dev;
	void __iomem *base;
	u32 clk_hz;
	unsigned int fifo;
};

/* where the next frame comes from or goes to in a group of transfers */
struct ffxav_cursor {
	struct spi_transfer *t;
	struct spi_transfer *end;	/* the one after the last */
	struct list_head *head;
	unsigned int pos;		/* bytes into t */
	unsigned int step;		/* bytes per frame */
	bool rx;
};

static inline u32 ssi_read(struct ffxav_spi *fs, unsigned int reg)
{
	return readl_relaxed(fs->base + reg);
}

static inline void ssi_write(struct ffxav_spi *fs, unsigned int reg, u32 v)
{
	writel_relaxed(v, fs->base + reg);
}

static void cursor_init(struct ffxav_cursor *c, struct spi_message *msg,
			struct spi_transfer *first, struct spi_transfer *end,
			unsigned int step, bool rx)
{
	c->t = first;
	c->end = end;
	c->head = &msg->transfers;
	c->pos = 0;
	c->step = step;
	c->rx = rx;
}

/* to the transfer that has the next frame */
static void cursor_settle(struct ffxav_cursor *c)
{
	while (c->t != c->end && c->pos >= c->t->len) {
		c->pos = 0;
		c->t = list_is_last(&c->t->transfer_list, c->head) ? NULL :
		       list_next_entry(c->t, transfer_list);
	}
}

static u32 cursor_get(struct ffxav_cursor *c)
{
	const void *buf;
	u32 v = 0;

	cursor_settle(c);
	buf = c->t->tx_buf;
	if (buf)
		v = c->step == 2 ? *(const u16 *)(buf + c->pos) :
				   *(const u8 *)(buf + c->pos);
	c->pos += c->step;
	return v;
}

static void cursor_put(struct ffxav_cursor *c, u32 v)
{
	void *buf;

	cursor_settle(c);
	buf = c->t->rx_buf;
	if (buf) {
		if (c->step == 2)
			*(u16 *)(buf + c->pos) = v;
		else
			*(u8 *)(buf + c->pos) = v;
	}
	c->pos += c->step;
}

static struct spi_transfer *next_xfer(struct spi_message *msg,
				      struct spi_transfer *t)
{
	return list_is_last(&t->transfer_list, &msg->transfers) ? NULL :
	       list_next_entry(t, transfer_list);
}

/*
 * One chip-select frame: the transfers first .. last. Returns 0 or an
 * error; the frames moved are added to msg->actual_length.
 */
static int ffxav_spi_group(struct ffxav_spi *fs, struct spi_message *msg,
			   struct spi_transfer *first, struct spi_transfer *last)
{
	struct spi_device *spi = msg->spi;
	struct spi_transfer *end = next_xfer(msg, last), *t, *rx_first = NULL;
	unsigned int step = first->bits_per_word > 8 ? 2 : 1;
	unsigned int tx_total = 0, rx_total = 0, tx_n = 0, rx_n = 0;
	unsigned int mode, baud, n, lvl, len = 0, spins = 0;
	struct ffxav_cursor tx, rx;
	bool only_tx = true, split = true, rx_seen = false;
	unsigned long flags;
	u64 deadline, ns;
	int ret = 0;
	u32 cr0;

	for (t = first; t != end; t = next_xfer(msg, t)) {
		len += t->len;
		if (t->rx_buf)
			only_tx = false;
		/* EEPROM read: transfers that only send, then that only read */
		if (t->rx_buf && !t->tx_buf) {
			if (!rx_seen)
				rx_first = t;
			rx_seen = true;
		} else if (rx_seen || t->rx_buf) {
			split = false;
		}
	}
	if (!len)
		return 0;

	if (only_tx) {
		mode = TMOD_TO;
		tx_total = len / step;
	} else if (split && rx_first && rx_first != first) {
		mode = TMOD_EEPROM;
		for (t = first; t != rx_first; t = next_xfer(msg, t))
			tx_total += t->len / step;
		rx_total = len / step - tx_total;
		if (!tx_total || rx_total > NDF_MAX)
			mode = TMOD_TR;
	} else {
		mode = TMOD_TR;
	}
	if (mode == TMOD_TR) {
		tx_total = len / step;
		rx_total = len / step;
		rx_first = first;
	}

	baud = DIV_ROUND_UP(fs->clk_hz, first->speed_hz);
	baud = clamp(round_up(baud, 2), 2u, 0xfffeu);
	cr0 = (first->bits_per_word - 1) | mode << CR0_TMOD_SHIFT;
	if (spi->mode & SPI_CPHA)
		cr0 |= CR0_CPHA;
	if (spi->mode & SPI_CPOL)
		cr0 |= CR0_CPOL;

	ssi_write(fs, SSI_SSIENR, 0);
	ssi_write(fs, SSI_CTRLR0, cr0);
	ssi_write(fs, SSI_CTRLR1, mode == TMOD_EEPROM ? rx_total - 1 : 0);
	ssi_write(fs, SSI_BAUDR, baud);
	ssi_write(fs, SSI_SER, 0);
	ssi_write(fs, SSI_IMR, 0);
	(void)ssi_read(fs, SSI_ICR);
	ssi_write(fs, SSI_SSIENR, 1);

	cursor_init(&tx, msg, first, end, step, false);
	cursor_init(&rx, msg, rx_first, end, step, true);

	/* the time the frames take on the wire, four times over, + 5 ms */
	ns = div_u64((u64)first->bits_per_word * baud * NSEC_PER_SEC,
		     fs->clk_hz) * (tx_total + rx_total);
	deadline = ktime_get_ns() + 4 * ns + 5 * NSEC_PER_MSEC;

	local_irq_save(flags);

	/* fill the FIFO while no chip select is set: nothing moves yet */
	n = min(fs->fifo, tx_total);
	for (; tx_n < n; tx_n++)
		ssi_write(fs, SSI_DR, cursor_get(&tx));
	ssi_write(fs, SSI_SER, BIT(spi_get_chipselect(spi, 0)));

	while (tx_n < tx_total || rx_n < rx_total) {
		if (mode != TMOD_TO && rx_n < rx_total) {
			lvl = ssi_read(fs, SSI_RXFLR);
			n = min(lvl, rx_total - rx_n);
			for (; n; n--, rx_n++)
				cursor_put(&rx, ssi_read(fs, SSI_DR));
			if (!lvl && (ssi_read(fs, SSI_RISR) & RISR_RXOI)) {
				ret = -EIO;
				break;
			}
		}
		if (tx_n < tx_total) {
			lvl = ssi_read(fs, SSI_TXFLR);
			/* (in EEPROM read mode an empty FIFO starts the read) */
			if (!lvl && (mode == TMOD_EEPROM ||
				     !(ssi_read(fs, SSI_SR) & SR_BUSY))) {
				ret = -EPIPE;	/* ran dry: chip select lost */
				break;
			}
			n = min(fs->fifo - lvl, tx_total - tx_n);
			/* full duplex: no more in flight than the FIFO holds */
			if (mode == TMOD_TR)
				n = min(n, fs->fifo - (tx_n - rx_n));
			for (; n; n--, tx_n++)
				ssi_write(fs, SSI_DR, cursor_get(&tx));
		}
		if (!(++spins % 64) && ktime_get_ns() > deadline) {
			ret = -ETIMEDOUT;
			break;
		}
	}

	/* the last frames out, then the chip select goes */
	while (!ret && (ssi_read(fs, SSI_SR) & (SR_TFE | SR_BUSY)) != SR_TFE) {
		if (ktime_get_ns() > deadline) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	ssi_write(fs, SSI_SER, 0);

	local_irq_restore(flags);

	if (ret)
		dev_err_ratelimited(fs->dev,
				    "cs %u mode %u: %s after %u/%u out, %u/%u in (sr %02x risr %02x)\n",
				    spi_get_chipselect(spi, 0), mode,
				    ret == -EIO ? "receive overrun" :
				    ret == -EPIPE ? "transmit FIFO ran dry" :
				    "timed out", tx_n, tx_total, rx_n,
				    rx_total, ssi_read(fs, SSI_SR),
				    ssi_read(fs, SSI_RISR));
	ssi_write(fs, SSI_SSIENR, 0);	/* also empties the FIFOs */
	if (!ret)
		msg->actual_length += len;
	return ret;
}

/*
 * Does this frame only read: something comes back and nothing it sends
 * changes the device (a flash read or status read, a SLIC register read)?
 * Such a frame is sent again after a receive overrun or a stall (the CPU
 * kept from the FIFO by other bus traffic, interrupts being off). A frame
 * that only sends (a page program, a register write) is never repeated.
 */
static bool group_reads(struct spi_transfer *first, struct spi_transfer *last)
{
	struct spi_transfer *t = first;

	for (;;) {
		if (t->rx_buf)
			return true;
		if (t == last)
			return false;
		t = list_next_entry(t, transfer_list);
	}
}

#define FFXAV_SPI_TRIES 3

static int ffxav_spi_transfer_one_message(struct spi_controller *ctlr,
					  struct spi_message *msg)
{
	struct ffxav_spi *fs = spi_controller_get_devdata(ctlr);
	struct spi_transfer *first, *last, *next;
	int ret = 0, tries;

	msg->actual_length = 0;
	first = list_first_entry_or_null(&msg->transfers, struct spi_transfer,
					 transfer_list);
	while (first) {
		/* as many as can share one chip-select frame */
		last = first;
		while (!last->cs_change && (next = next_xfer(msg, last)) &&
		       next->bits_per_word == first->bits_per_word &&
		       next->speed_hz == first->speed_hz &&
		       !last->delay.value && !last->word_delay.value)
			last = next;

		for (tries = 1; ; tries++) {
			ret = ffxav_spi_group(fs, msg, first, last);
			if (!ret || tries >= FFXAV_SPI_TRIES ||
			    !group_reads(first, last))
				break;
		}
		if (ret)
			break;
		if (tries > 1)
			dev_warn_ratelimited(fs->dev, "read went through on try %d\n", tries);
		spi_transfer_delay_exec(last);
		next = next_xfer(msg, last);
		if (next && last->cs_change)
			spi_delay_exec(&last->cs_change_delay, last);
		first = next;
	}

	msg->status = ret;
	spi_finalize_current_message(ctlr);
	return ret;
}

static size_t ffxav_spi_max_transfer_size(struct spi_device *spi)
{
	return clamp_t(unsigned int, max_xfer, FFXAV_SPI_FIFO, NDF_MAX);
}

/* room for a command, an address and dummy bytes besides the data */
static size_t ffxav_spi_max_message_size(struct spi_device *spi)
{
	return ffxav_spi_max_transfer_size(spi) + 16;
}

/*
 * The FIFO's depth, the way the DesignWare driver finds it: the transmit
 * threshold takes values below the depth only. Done with the controller
 * disabled; the threshold is only used for interrupts, which we do not use.
 */
static unsigned int ffxav_spi_fifo_depth(struct ffxav_spi *fs)
{
	u32 old = ssi_read(fs, SSI_TXFTLR);
	unsigned int n;

	for (n = 1; n < 256; n++) {
		ssi_write(fs, SSI_TXFTLR, n);
		if (ssi_read(fs, SSI_TXFTLR) != n)
			break;
	}
	ssi_write(fs, SSI_TXFTLR, old);
	return n;
}

static int ffxav_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct ffxav_spi *fs;
	struct resource *res;
	unsigned int depth;
	u32 sysv = 0;

	ctlr = devm_spi_alloc_host(dev, sizeof(*fs));
	if (!ctlr)
		return -ENOMEM;
	fs = spi_controller_get_devdata(ctlr);
	fs->dev = dev;
	fs->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fs->base))
		return PTR_ERR(fs->base);
	if (of_property_read_u32(dev->of_node, "clock-frequency", &fs->clk_hz))
		fs->clk_hz = FFXAV_SPI_CLK;

	dev_info(dev, "ctrlr0 %04x ctrlr1 %04x ssienr %x ser %x baudr %04x txftlr %x rxftlr %x txflr %x rxflr %x sr %02x imr %02x risr %02x idr %08x version %08x\n",
		 ssi_read(fs, SSI_CTRLR0), ssi_read(fs, SSI_CTRLR1),
		 ssi_read(fs, SSI_SSIENR), ssi_read(fs, SSI_SER),
		 ssi_read(fs, SSI_BAUDR), ssi_read(fs, SSI_TXFTLR),
		 ssi_read(fs, SSI_RXFTLR), ssi_read(fs, SSI_TXFLR),
		 ssi_read(fs, SSI_RXFLR), ssi_read(fs, SSI_SR),
		 ssi_read(fs, SSI_IMR), ssi_read(fs, SSI_RISR),
		 ssi_read(fs, SSI_IDR), ssi_read(fs, SSI_VERSION));

	/* the system register (not requested: other code uses it too) */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		void __iomem *sys = devm_ioremap(dev, res->start,
						 resource_size(res));

		if (!sys)
			return -ENOMEM;
		sysv = readl(sys);
		writel(readl(sys) & ~SYS_SPI_CLR, sys);
		writel(readl(sys) | SYS_SPI_SET, sys);
		udelay(125);
		dev_info(dev, "system register %pa: %08x -> %08x\n",
			 &res->start, sysv, readl(sys));
	}

	ssi_write(fs, SSI_SSIENR, 0);
	ssi_write(fs, SSI_SER, 0);
	ssi_write(fs, SSI_IMR, 0);
	depth = ffxav_spi_fifo_depth(fs);
	fs->fifo = depth >= 2 ? min_t(unsigned int, depth, FFXAV_SPI_FIFO) :
				FFXAV_SPI_FIFO;

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = FFXAV_SPI_NCS;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16);
	ctlr->max_speed_hz = fs->clk_hz / 2;
	ctlr->min_speed_hz = DIV_ROUND_UP(fs->clk_hz, 0xfffe);
	ctlr->transfer_one_message = ffxav_spi_transfer_one_message;
	ctlr->max_transfer_size = ffxav_spi_max_transfer_size;
	ctlr->max_message_size = ffxav_spi_max_message_size;

	dev_info(dev, "%u MHz, FIFO %u deep (using %u), transfers up to %zu bytes\n",
		 fs->clk_hz / 1000000, depth, fs->fifo,
		 ffxav_spi_max_transfer_size(NULL));
	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id ffxav_spi_of_match[] = {
	{ .compatible = "marvell,ffxav-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, ffxav_spi_of_match);

static struct platform_driver ffxav_spi_driver = {
	.probe = ffxav_spi_probe,
	.driver = {
		.name = "ffxav-spi",
		.of_match_table = ffxav_spi_of_match,
	},
};
module_platform_driver(ffxav_spi_driver);

MODULE_DESCRIPTION("Marvell FFxAV SPI controller");
MODULE_LICENSE("GPL");
