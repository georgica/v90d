// SPDX-License-Identifier: GPL-2.0
/*
 * The telephone side of the Obihai OBi200 (Marvell 88E7251 "FFxAV"), with
 * the user interface of the board's own telephony module (ffxav_mp.ko of
 * the maker's Linux 2.6.30 firmware) as far as v90d's line helper
 * (fw/obi/obiline.c) uses it, so that program runs unchanged here:
 *
 *   /dev/fxs0, /dev/fxs1  the two channels of the Si3226x SLIC (channel 0
 *       is the jack). ioctls (all need CAP_SYS_ADMIN, else EPERM, as there):
 *         0xc00171b2  read a SLIC register: arg[0] the register, the value
 *                     comes back in arg[4]
 *         0x400471b3  write one: arg[0] the register, arg[4] the value
 *         0x7191, 0x7192  nothing (the maker's application issues 0x7191
 *                     before starting a channel)
 *         0x718a      start the channel's TDM, both directions
 *         0x718b      stop it
 *         0x71b6      the SLIC's reset line: 0 holds the part in reset,
 *                     anything else lets it go. It is held from probe on.
 *       One reader and one writer per device (EBUSY), as there.
 *   /dev/pcm  both channels, 10 ms at a time: a read gives 320 octets, the
 *       80 16-bit samples (host order, little endian) of channel 0 and then
 *       the 80 of channel 1; a write takes the same. poll gives IN and OUT
 *       together once per 10 ms, when a block has come in and one may go
 *       out; nothing is written ahead. Three blocks are kept each way.
 *
 * Where this differs from the board's module (TDM-NOTES.md): a read with
 * nothing there waits (or EAGAIN) instead of returning old data; a receive
 * overrun drops the new block and a transmit underrun sends silence, and
 * neither stops the channel; opening and closing /dev/fxsN does not touch
 * the SLIC's interrupt registers; the tone, ring and DAA ioctls are ENOTTY.
 *
 * The SLIC is a device on the SPI bus (child node of the SPI controller,
 * compatible "silabs,si3226x-obi"), addressed as the board's module does:
 * chip select 2, mode 3, 2 MHz, 8-bit words, the channel in the CID bits
 * of the control octet (0x10 for channel 1). The TDM highway is the SoC's
 * audio block at 0x80018000 (compatible "marvell,ffxav-tdm"), which owns
 * the character devices and the reset line.
 *
 * Written for the v90d project from the maker's binaries as they run; no
 * vendor code.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

/* ---- the ioctls of ffxav_mp.ko ---- */
#define FXS_REG_RD	0xc00171b2u
#define FXS_REG_WR	0x400471b3u
#define FXS_TDM_PREP	0x7191u
#define FXS_TDM_PREP2	0x7192u
#define FXS_TDM_ON	0x718au
#define FXS_TDM_OFF	0x718bu
#define FXS_RESET	0x71b6u

#define CHANS		2
#define PERIOD		80			/* samples per block: 10 ms */
#define BLOCK		(CHANS * PERIOD * 2)	/* octets per read or write */
#define RING		3			/* blocks kept each way */
#define READY_MAX	2			/* as the module's ready counters */

/* ---- the SLIC on the SPI bus ---- */

static int spi_framing;
module_param(spi_framing, int, 0644);
MODULE_PARM_DESC(spi_framing,
	"SLIC access: 0 as the board's module (default), 1 one octet per chip select, 2 three octets full duplex");

struct tel_slic {
	struct spi_device *spi;
	struct mutex lock;	/* the buffers below */
	u8 *tx, *rx;		/* DMA-safe */
	unsigned long reads, writes, errors;
};

static DEFINE_MUTEX(slic_mutex);	/* the_slic */
static struct tel_slic *the_slic;

static u8 slic_ctl(int ch, bool rd)
{
	return (rd ? 0x60 : 0x20) | (ch ? 0x10 : 0);
}

static int slic_read(struct tel_slic *s, int ch, u8 reg, u8 *val)
{
	struct spi_transfer t[3] = { };
	int r, n;

	mutex_lock(&s->lock);
	switch (spi_framing) {
	case 1:
		/* control, address, then the data octet: each its own frame */
		s->tx[0] = slic_ctl(ch, true);
		s->tx[1] = reg;
		s->tx[2] = 0xff;
		t[0].tx_buf = s->tx;
		t[0].len = 1;
		t[0].cs_change = 1;
		t[1].tx_buf = s->tx + 1;
		t[1].len = 1;
		t[1].cs_change = 1;
		t[2].tx_buf = s->tx + 2;
		t[2].rx_buf = s->rx;
		t[2].len = 1;
		n = 3;
		break;
	case 2:
		s->tx[0] = slic_ctl(ch, true);
		s->tx[1] = reg;
		s->tx[2] = 0xff;
		t[0].tx_buf = s->tx;
		t[0].rx_buf = s->rx;
		t[0].len = 3;
		n = 1;
		break;
	default:
		/*
		 * The board's module (vm=2): three octets out, the control,
		 * the register and a 0, then one octet in, in one message.
		 */
		s->tx[0] = slic_ctl(ch, true);
		s->tx[1] = reg;
		s->tx[2] = 0;
		t[0].tx_buf = s->tx;
		t[0].len = 3;
		t[1].rx_buf = s->rx;
		t[1].len = 1;
		n = 2;
		break;
	}
	r = spi_sync_transfer(s->spi, t, n);
	*val = spi_framing == 2 ? s->rx[2] : s->rx[0];
	s->reads++;
	if (r)
		s->errors++;
	mutex_unlock(&s->lock);
	return r;
}

static int slic_write(struct tel_slic *s, int ch, u8 reg, u8 val)
{
	struct spi_transfer t[3] = { };
	int r, n, i;

	mutex_lock(&s->lock);
	s->tx[0] = slic_ctl(ch, false);
	s->tx[1] = reg;
	s->tx[2] = val;
	s->tx[3] = 0;
	switch (spi_framing) {
	case 1:
		for (i = 0; i < 3; i++) {
			t[i].tx_buf = s->tx + i;
			t[i].len = 1;
			t[i].cs_change = i < 2;
		}
		n = 3;
		break;
	case 2:
		t[0].tx_buf = s->tx;
		t[0].len = 3;
		n = 1;
		break;
	default:
		/* the board's module (vm=2): control, register, value, 0 */
		t[0].tx_buf = s->tx;
		t[0].len = 4;
		n = 1;
		break;
	}
	r = spi_sync_transfer(s->spi, t, n);
	s->writes++;
	if (r)
		s->errors++;
	mutex_unlock(&s->lock);
	return r;
}

static int tel_slic_probe(struct spi_device *spi)
{
	struct tel_slic *s;
	int r;

	s = devm_kzalloc(&spi->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->tx = devm_kzalloc(&spi->dev, 8, GFP_KERNEL);
	s->rx = devm_kzalloc(&spi->dev, 8, GFP_KERNEL);
	if (!s->tx || !s->rx)
		return -ENOMEM;
	mutex_init(&s->lock);
	s->spi = spi;
	/* what the board's module asks for, unless the tree says otherwise */
	if (!spi->max_speed_hz)
		spi->max_speed_hz = 2000000;
	spi->bits_per_word = 8;
	r = spi_setup(spi);
	if (r)
		return dev_err_probe(&spi->dev, r, "spi_setup\n");
	spi_set_drvdata(spi, s);
	mutex_lock(&slic_mutex);
	the_slic = s;
	mutex_unlock(&slic_mutex);
	dev_info(&spi->dev, "SLIC on chip select %u, mode %u, %u Hz\n",
		 spi_get_chipselect(spi, 0),
		 (unsigned int)(spi->mode & SPI_MODE_X_MASK),
		 spi->max_speed_hz);
	return 0;
}

static void tel_slic_remove(struct spi_device *spi)
{
	mutex_lock(&slic_mutex);
	the_slic = NULL;
	mutex_unlock(&slic_mutex);
}

static const struct of_device_id tel_slic_of_match[] = {
	{ .compatible = "silabs,si3226x-obi" },
	{ }
};
MODULE_DEVICE_TABLE(of, tel_slic_of_match);

static const struct spi_device_id tel_slic_ids[] = {
	{ "si3226x-obi" },
	{ }
};
MODULE_DEVICE_TABLE(spi, tel_slic_ids);

static struct spi_driver tel_slic_driver = {
	.probe = tel_slic_probe,
	.remove = tel_slic_remove,
	.id_table = tel_slic_ids,
	.driver = {
		.name = "si3226x-obi",
		.of_match_table = tel_slic_of_match,
	},
};

/* ---- the blocks of /dev/pcm ---- */

/*
 * Where each channel's 16 bits are in a 32-octet TDM frame as the audio
 * block stores it, most significant octet first (the board's module:
 * channel 0 at octet 0, channel 1 at octet 10, 16-bit linear). Module
 * parameters, for a hardware test that finds them elsewhere.
 */
static unsigned int chan_off[CHANS] = { 0, 10 };
module_param_array(chan_off, uint, NULL, 0644);
MODULE_PARM_DESC(chan_off, "octet of each channel's MSB in a 32-octet frame (0,10)");
static bool lsb_first;
module_param(lsb_first, bool, 0644);
MODULE_PARM_DESC(lsb_first, "a sample's low octet comes first in the frame");

struct tel_fxs {
	struct tel *t;
	int ch;
	char name[8];
	struct miscdevice misc;
	bool reader, writer;	/* under t->open_lock */
};

struct tel {
	struct device *dev;
	void __iomem *regs;
	struct gpio_desc *slic_reset;

	spinlock_t lock;		/* everything below up to stats */
	bool rx_on[CHANS], tx_on[CHANS];
	u8 rx[RING][BLOCK];		/* received blocks, oldest at rx_r */
	unsigned int rx_r, rx_n;
	u8 tx[RING][BLOCK];		/* blocks to send, oldest at tx_r */
	unsigned int tx_r, tx_n;
	unsigned int tx_credit;		/* blocks that may be written */
	wait_queue_head_t wq;

	struct {
		unsigned long irqs, frames, blocks, rx_overruns, tx_underruns,
			      tx_overwrites, reads, writes, hw_errors;
	} st;

	struct mutex open_lock;		/* the open flags */
	bool pcm_reader, pcm_writer;
	struct tel_fxs fxs[CHANS];
	struct miscdevice pcm;

	struct tel_hw *hw;		/* the audio block's state */
};

static bool any(const bool *on)
{
	return on[0] || on[1];
}

/* both directions ready: as the module's poll, once per block */
static bool tel_ready(struct tel *t)
{
	return (!any(t->rx_on) || t->rx_n) &&
	       (!any(t->tx_on) || t->tx_credit) &&
	       (any(t->rx_on) || any(t->tx_on));
}

#define FRAME		32			/* octets per TDM frame */

static unsigned int off_ms(unsigned int c)
{
	return min(chan_off[c], FRAME - 2) + (lsb_first ? 1 : 0);
}

static unsigned int off_ls(unsigned int c)
{
	return min(chan_off[c], FRAME - 2) + (lsb_first ? 0 : 1);
}

/*
 * 80 frames have come in (one DMA descriptor, 10 ms), under t->lock from
 * the interrupt: a block for the reader, and the pace of /dev/pcm.
 */
static void tel_rx_block(struct tel *t, const u8 *d)
{
	unsigned int c, i;

	t->st.frames += PERIOD;
	t->st.blocks++;
	if (any(t->rx_on)) {
		if (t->rx_n == RING) {
			t->st.rx_overruns++;	/* the old ones stay */
		} else {
			u8 *b = t->rx[(t->rx_r + t->rx_n) % RING];

			for (c = 0; c < CHANS; c++) {
				u8 *r = b + 2 * c * PERIOD;
				unsigned int m = off_ms(c), l = off_ls(c);

				if (!t->rx_on[c]) {
					memset(r, 0, 2 * PERIOD);
					continue;
				}
				for (i = 0; i < PERIOD; i++) {
					const u8 *f = d + i * FRAME;

					r[2 * i] = f[l];
					r[2 * i + 1] = f[m];
				}
			}
			t->rx_n++;
		}
	}
	if (any(t->tx_on) && t->tx_credit < READY_MAX)
		t->tx_credit++;
	wake_up_interruptible(&t->wq);
}

/* 80 frames to send (a DMA descriptor the hardware plays soon) */
static void tel_tx_block(struct tel *t, u8 *d)
{
	const u8 *b = NULL;
	unsigned int c, i;

	if (t->tx_n) {
		b = t->tx[t->tx_r];
		t->tx_r = (t->tx_r + 1) % RING;
		t->tx_n--;
	} else if (any(t->tx_on) && t->st.writes) {
		t->st.tx_underruns++;
	}
	for (c = 0; c < CHANS; c++) {
		const u8 *x = b ? b + 2 * c * PERIOD : NULL;
		unsigned int m = off_ms(c), l = off_ls(c);

		for (i = 0; i < PERIOD; i++) {
			u8 *f = d + i * FRAME;

			if (x && t->tx_on[c]) {
				f[l] = x[2 * i];
				f[m] = x[2 * i + 1];
			} else {
				f[l] = 0;
				f[m] = 0;
			}
		}
	}
}

static void tel_chan(struct tel *t, int ch, bool on)
{
	unsigned long flags;

	spin_lock_irqsave(&t->lock, flags);
	if (on && !t->rx_on[ch] && !t->tx_on[ch] && !any(t->rx_on) &&
	    !any(t->tx_on)) {
		/* the first channel: start with empty rings */
		t->rx_n = t->tx_n = 0;
		t->tx_credit = 0;
	}
	t->rx_on[ch] = on;
	t->tx_on[ch] = on;
	spin_unlock_irqrestore(&t->lock, flags);
	wake_up_interruptible(&t->wq);
}

static ssize_t pcm_read(struct file *f, char __user *buf, size_t n, loff_t *off)
{
	struct tel *t = container_of(f->private_data, struct tel, pcm);
	u8 *b;
	int r;

	if (n < BLOCK)
		return -EINVAL;
	for (;;) {
		spin_lock_irq(&t->lock);
		if (any(t->rx_on) && t->rx_n)
			break;
		spin_unlock_irq(&t->lock);
		if (f->f_flags & O_NONBLOCK)
			return any(t->rx_on) ? -EAGAIN : -EPERM;
		r = wait_event_interruptible(t->wq,
			READ_ONCE(t->rx_n) && any(t->rx_on));
		if (r)
			return r;
	}
	b = t->rx[t->rx_r];
	t->rx_r = (t->rx_r + 1) % RING;
	t->rx_n--;
	t->st.reads++;
	spin_unlock_irq(&t->lock);
	/*
	 * The block stays in the ring until three more have come in: 30 ms
	 * for copy_to_user, which runs with the lock released.
	 */
	if (copy_to_user(buf, b, BLOCK))
		return -EFAULT;
	return BLOCK;
}

static ssize_t pcm_write(struct file *f, const char __user *buf, size_t n,
			 loff_t *off)
{
	struct tel *t = container_of(f->private_data, struct tel, pcm);
	u8 tmp[BLOCK];

	if (n < BLOCK)
		return -EINVAL;
	if (copy_from_user(tmp, buf, BLOCK))
		return -EFAULT;
	spin_lock_irq(&t->lock);
	if (!any(t->tx_on)) {
		spin_unlock_irq(&t->lock);
		return -EPERM;
	}
	if (t->tx_n == RING) {
		/* written ahead of the clock: the oldest one goes */
		t->tx_r = (t->tx_r + 1) % RING;
		t->tx_n--;
		t->st.tx_overwrites++;
	}
	memcpy(t->tx[(t->tx_r + t->tx_n) % RING], tmp, BLOCK);
	t->tx_n++;
	if (t->tx_credit)
		t->tx_credit--;
	t->st.writes++;
	spin_unlock_irq(&t->lock);
	return BLOCK;
}

static __poll_t pcm_poll(struct file *f, poll_table *pt)
{
	struct tel *t = container_of(f->private_data, struct tel, pcm);
	__poll_t m = 0;

	poll_wait(f, &t->wq, pt);
	spin_lock_irq(&t->lock);
	if (tel_ready(t)) {
		if (any(t->rx_on))
			m |= EPOLLIN | EPOLLRDNORM;
		if (any(t->tx_on))
			m |= EPOLLOUT | EPOLLWRNORM;
	}
	spin_unlock_irq(&t->lock);
	return m;
}

/* one reader and one writer, as the board's module allows */
static int tel_claim(struct tel *t, struct file *f, bool *rd, bool *wr)
{
	int r = 0;

	mutex_lock(&t->open_lock);
	if (((f->f_mode & FMODE_READ) && *rd) ||
	    ((f->f_mode & FMODE_WRITE) && *wr)) {
		r = -EBUSY;
	} else {
		if (f->f_mode & FMODE_READ)
			*rd = true;
		if (f->f_mode & FMODE_WRITE)
			*wr = true;
	}
	mutex_unlock(&t->open_lock);
	return r;
}

static void tel_unclaim(struct tel *t, struct file *f, bool *rd, bool *wr)
{
	mutex_lock(&t->open_lock);
	if (f->f_mode & FMODE_READ)
		*rd = false;
	if (f->f_mode & FMODE_WRITE)
		*wr = false;
	mutex_unlock(&t->open_lock);
}

static int pcm_open(struct inode *ino, struct file *f)
{
	struct tel *t = container_of(f->private_data, struct tel, pcm);

	return tel_claim(t, f, &t->pcm_reader, &t->pcm_writer);
}

static int pcm_release(struct inode *ino, struct file *f)
{
	struct tel *t = container_of(f->private_data, struct tel, pcm);

	tel_unclaim(t, f, &t->pcm_reader, &t->pcm_writer);
	wake_up_interruptible(&t->wq);
	return 0;
}

static const struct file_operations pcm_fops = {
	.owner = THIS_MODULE,
	.open = pcm_open,
	.release = pcm_release,
	.read = pcm_read,
	.write = pcm_write,
	.poll = pcm_poll,
	.llseek = noop_llseek,
};

/* ---- /dev/fxs0, /dev/fxs1 ---- */

static long fxs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct tel_fxs *x = container_of(f->private_data, struct tel_fxs, misc);
	struct tel *t = x->t;
	u8 __user *a = (u8 __user *)arg;
	u8 reg, val;
	int r;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	switch (cmd) {
	case FXS_REG_RD:
	case FXS_REG_WR:
		if (get_user(reg, a))
			return -EFAULT;
		if (cmd == FXS_REG_WR && get_user(val, a + 4))
			return -EFAULT;
		mutex_lock(&slic_mutex);
		if (!the_slic)
			r = -ENODEV;
		else if (cmd == FXS_REG_RD)
			r = slic_read(the_slic, x->ch, reg, &val);
		else
			r = slic_write(the_slic, x->ch, reg, val);
		mutex_unlock(&slic_mutex);
		if (r)
			return r;
		if (cmd == FXS_REG_RD && put_user(val, a + 4))
			return -EFAULT;
		return 0;
	case FXS_TDM_PREP:
	case FXS_TDM_PREP2:
		return 0;
	case FXS_TDM_ON:
	case FXS_TDM_OFF:
		tel_chan(t, x->ch, cmd == FXS_TDM_ON);
		return 0;
	case FXS_RESET:
		/* one line for the part's two channels, as on the board */
		if (!t->slic_reset)
			return -ENODEV;
		dev_info(t->dev, "%s SLIC chip\n", arg ? "UnReset" : "Reset");
		gpiod_set_value_cansleep(t->slic_reset, !arg);
		return 0;
	default:
		dev_dbg(t->dev, "fxs%d: ioctl %08x not offered\n", x->ch, cmd);
		return -ENOTTY;
	}
}

static int fxs_open(struct inode *ino, struct file *f)
{
	struct tel_fxs *x = container_of(f->private_data, struct tel_fxs, misc);

	return tel_claim(x->t, f, &x->reader, &x->writer);
}

static int fxs_release(struct inode *ino, struct file *f)
{
	struct tel_fxs *x = container_of(f->private_data, struct tel_fxs, misc);

	/* the module resets the channel's TDM on every close */
	tel_chan(x->t, x->ch, false);
	tel_unclaim(x->t, f, &x->reader, &x->writer);
	return 0;
}

static const struct file_operations fxs_fops = {
	.owner = THIS_MODULE,
	.open = fxs_open,
	.release = fxs_release,
	.unlocked_ioctl = fxs_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

/* ---- the audio block (TDM-NOTES.md, "The audio block") ---- */

/* port 0 at 0x80018000 */
#define A_CTRL		0x000	/* bit 3 shared port, bit 8 volume on */
#define A_CLK		0x004	/* rate, multipliers, PLL lock (bit 24) */
#define A_CAUSE		0x008	/* write 1 to clear */
#define A_MASK		0x00c
#define A_CHAN		0x010	/* channels per frame, enables, running */
#define A_HPT_CAL	0x040
#define A_TX		0x100	/* TX sub-block */
#define A_RX		0x200	/* RX sub-block */
#define A_FORMAT	0x008	/* in a sub-block */
#define A_TDM		0x00c	/* in a sub-block */
#define TX_CAUSES	0x00070f05
#define RX_CAUSES	0x0030000a
#define ALL_CAUSES	0x00370f0f
#define CHAN_TX_EN	BIT(15)
#define CHAN_TX_RUN	BIT(14)
#define CHAN_RX_EN	BIT(31)
#define CHAN_RX_RUN	BIT(30)

/* global block at 0x8001b000 (offsets as the addresses' low 12 bits) */
#define G_ID		0x400
#define G_PTP		0x408
#define G_CAUSE		0x410
#define G_MASK		0x414
#define G_PERR_CAUSE	0x418
#define G_PERR_MASK	0x41c
#define G_DMA_CAUSE	0x438	/* bit 0 TX done, bit 16 RX done (port 0) */
#define G_DMA_MASK	0x43c
#define G_ROUTE		0x500	/* port 0 clock route, bit 31 soft reset */
#define G_DMA_CFG	0x800
#define G_RING_VALID	0x804
#define G_RING_CMD	0x80c
#define G_RING(r)	(0x880 + (r) * 0x20)
#define R_CFG		0x00
#define R_PTR		0x04
#define R_DONE		0x08
#define R_BASE		0x0c
#define R_THRESH	0x10
#define G_ERR_SUM	BIT(29)

/* system block at 0x8000f000 */
#define S_STRAP		0x094
#define S_PLL_ST	0x0a8
#define S_CLK		0x0ac
#define S_PLL_KICK	0x0b0
#define S_PADS		0x0dc
#define S_CLK_AUDIO	0x00207f80

#define NDESC		16
#define SEG		(PERIOD * FRAME)	/* 0xa00: one descriptor, 10 ms */
#define RINGB		(NDESC * SEG)
#define DESC_W0		(BIT(21) | (SEG / 4))
#define RING_TX		0
#define RING_RX		1

static unsigned int tx_lead = 1;
module_param(tx_lead, uint, 0644);
MODULE_PARM_DESC(tx_lead, "descriptors between the one playing and the one filled (1..3, the module's 1)");

struct tel_hw {
	void __iomem *port, *glob, *sys;
	int irq;
	u8 *buf[2];		/* TX, RX data: 16 descriptors of 80 frames */
	dma_addr_t buf_dma[2];
	u32 *desc[2];		/* 16 descriptors of 8 words */
	dma_addr_t desc_dma[2];
	unsigned int tx_played;	/* TX descriptors completed */
	unsigned int tx_prod;	/* TX producer index */
	unsigned int rx_next;	/* next RX descriptor to come in */
	bool running;
	bool adopted;		/* rings taken over from the stock kernel */
	bool err_masked;	/* port errors came too often: masked */
	unsigned long win;	/* start of the pacing window (jiffies) */
	unsigned int win_n;	/* descriptors completed in it */
	unsigned long settle;	/* the pacing guard is off until then */
	bool unpaced;		/* they came far too fast: DMA masked */
};

static bool adopt_restart;
module_param(adopt_restart, bool, 0444);
MODULE_PARM_DESC(adopt_restart, "when taking over running rings, also stop and start the TX and RX channels (as the stock Start does)");

static void rmw(void __iomem *a, u32 clr, u32 set)
{
	writel((readl(a) & ~clr) | set, a);
}

static int poll_clear(struct tel *t, void __iomem *a, u32 bit, const char *what)
{
	u32 v;
	int r = readl_poll_timeout_atomic(a, v, !(v & bit), 1, 20000);

	if (r)
		dev_err(t->dev, "%s: %08x stays\n", what, v);
	return r;
}

/* the ring enable command (audioDmaRingEnable) */
static int ring_enable(struct tel *t, int r, bool on)
{
	struct tel_hw *h = t->hw;
	void __iomem *cmd = h->glob + G_RING_CMD;
	u32 v;

	writel(0x82100000, cmd);
	if (poll_clear(t, cmd, BIT(31), "ring status"))
		return -ETIMEDOUT;
	v = readl(cmd);
	v = (v & ~0x0f000000) | 0x83000000;
	v = on ? v | BIT(r) : v & ~BIT(r);
	writel(v, cmd);
	if (poll_clear(t, cmd, BIT(31), "ring enable"))
		return -ETIMEDOUT;
	if (!on)
		writel(0, h->glob + G_RING(r) + R_PTR);
	return 0;
}

static int port_reset(struct tel *t)
{
	void __iomem *a = t->hw->glob + G_ROUTE;

	rmw(a, 0, BIT(31));
	return poll_clear(t, a, BIT(31), "port reset");
}

static void clk_mult(struct tel *t, u32 mask, u32 val)
{
	void __iomem *a = t->hw->port + A_CLK;
	u32 v = readl(a) & ~BIT(31);

	writel(v, a);
	v = (v & ~mask) | val;
	writel(v, a);
	writel(v | BIT(31), a);
}

/* mvAudioIntfSetHwConfig {10, 8, 32, 8000, 1} for one direction */
static int port_config(struct tel *t, bool rx)
{
	struct tel_hw *h = t->hw;
	void __iomem *sub = h->port + (rx ? A_RX : A_TX);
	void __iomem *clk = h->port + A_CLK;
	u32 v;
	int r;

	if (!rx)
		rmw(h->port + A_CTRL, BIT(3), 0);
	/* 8 kHz (code 0x43), then wait for the port's PLL */
	v = readl(clk) & ~0xc0000000;
	writel(v, clk);
	v = (v & ~0xff) | 0x43;
	writel(v, clk);
	v |= 0xe0000000;
	writel(v, clk);
	v &= ~0x20000000;
	writel(v, clk);
	r = readl_poll_timeout_atomic(clk, v, v & BIT(24), 10, 100000);
	if (r) {
		dev_err(t->dev, "audio PLL does not lock: %08x\n", v);
		return r;
	}
	/* 8 slots of 32 bits, 256 bit clocks a frame, 1-clock frame sync */
	writel(0x000007a5, sub + A_FORMAT);
	v = readl(sub + A_TDM) | BIT(31);
	writel(v, sub + A_TDM);
	v = (v & 0xf008000f) | 0x00060ff0;
	writel(v, sub + A_TDM);
	writel(v & ~BIT(31), sub + A_TDM);
	if (rx)
		rmw(h->port + A_CHAN, 0x3f0000, 8 << 16);
	else
		rmw(h->port + A_CHAN, 0x3f, 8);
	clk_mult(t, 0x700, 2 << 8);		/* bit clock 256 fs */
	clk_mult(t, 0x70000, 2 << 16);		/* master clock 256 fs */
	rmw(sub + A_TDM, 0, BIT(2));		/* FIFO flush */
	rmw(sub + A_TDM, BIT(2), 0);
	rmw(sub + A_TDM, 0, BIT(1));		/* reset */
	rmw(sub + A_TDM, BIT(1), 0);
	writel(0, h->port + A_HPT_CAL);
	return 0;
}

/* mvAudioIntfSetupDma(0xa000, 16, 16, 80) for one direction */
static int ring_setup(struct tel *t, bool rx)
{
	struct tel_hw *h = t->hw;
	int r = rx ? RING_RX : RING_TX, i;
	void __iomem *rr = h->glob + G_RING(r);
	u32 *d = h->desc[rx];

	if (ring_enable(t, r, false))
		return -ETIMEDOUT;
	writel(0, rr + R_PTR);
	writel((readl(rr + R_CFG) & 0x083f) | (SEG << 16) | (rx ? BIT(15) : 0) |
	       0x3000, rr + R_CFG);
	for (i = 0; i < NDESC; i++, d += 8) {
		d[0] = DESC_W0;
		d[1] = 0;
		d[2] = h->buf_dma[rx] + i * SEG;
		d[3] = 0;
		d[4] = 0;
		d[5] = (u32)(uintptr_t)(h->buf[rx] + i * SEG);	/* a cookie */
		d[6] = 0;
		d[7] = 0;
	}
	wmb();	/* the descriptors before the ring's base */
	writel(h->desc_dma[rx], rr + R_BASE);
	rmw(h->glob + G_RING_VALID, 0, BIT(r));
	return ring_enable(t, r, true);
}

/* once: what audioInit does at boot, for a cold start */
static int audio_init(struct tel *t)
{
	struct tel_hw *h = t->hw;
	void __iomem *route = h->glob + G_ROUTE;
	u32 v, mask;

	writel(0x83100000, h->glob + G_RING_CMD);	/* every ring off */
	if (poll_clear(t, h->glob + G_RING_CMD, BIT(31), "rings off"))
		return -ETIMEDOUT;
	writel((readl(h->glob + G_DMA_CFG) & BIT(31)) | 0x00000e70,
	       h->glob + G_DMA_CFG);
	writel(0, h->glob + G_RING_VALID);
	writel(0, h->glob + 0x810);
	writel(0, h->glob + 0x814);
	/* port 0 takes its own clocks, both ways */
	v = readl(route);
	writel(v & ~0x80, route);
	v &= ~0x87;
	writel(v, route);
	writel(v | 0x80, route);
	v = readl(route);
	writel(v & ~0x8000, route);
	v &= ~0x8700;
	writel(v, route);
	writel(v | 0x8000, route);
	/*
	 * The audio clocks of the system block: the stock kernel's boot
	 * sequence, only if they are not set up already (a RAM boot from the
	 * stock system has them).
	 */
	mask = S_CLK_AUDIO;
	if (!(readl(h->sys + S_STRAP) & BIT(4)))
		mask &= ~0x6000;
	if ((readl(h->sys + S_CLK) & mask) != mask) {
		static const u32 grp[] = { 0x6000, 0x600, 0x1800, 0x180 };
		int i, r;

		dev_info(t->dev, "audio clocks: %08x, starting them\n",
			 readl(h->sys + S_CLK));
		for (i = 0; i < ARRAY_SIZE(grp); i++) {
			if (!(grp[i] & mask))
				continue;
			rmw(h->sys + S_CLK, grp[i], 0);
			rmw(h->sys + S_CLK, 0, grp[i]);
		}
		rmw(h->sys + S_CLK, 0, 0x200000);
		rmw(h->sys + S_PLL_KICK, 0, BIT(4));
		r = readl_poll_timeout_atomic(h->sys + S_PLL_ST, v, v & BIT(16),
					      10, 100000);
		rmw(h->sys + S_PLL_KICK, BIT(4), 0);
		if (r)
			dev_warn(t->dev, "audio clock PLL: %08x\n", v);
	}
	rmw(h->glob + G_MASK, 0, 0xa3000000);
	rmw(h->glob + G_PTP, 1, 1);
	return 0;
}

static int audio_start(struct tel *t)
{
	struct tel_hw *h = t->hw;
	int r, i;

	/* TX open, configuration, ring */
	rmw(h->port + A_CTRL, BIT(3), 0);
	if (ring_enable(t, RING_TX, false))
		return -ETIMEDOUT;
	r = port_reset(t);
	if (r)
		return r;
	rmw(h->port + A_CTRL, 0, BIT(8));
	r = port_config(t, false);
	if (!r)
		r = ring_setup(t, false);
	if (r)
		return r;
	/* RX open (a second port reset, as the stock kernel does), ring */
	if (ring_enable(t, RING_RX, false))
		return -ETIMEDOUT;
	r = port_reset(t);
	if (r)
		return r;
	rmw(h->port + A_CTRL, 0, BIT(8));
	r = port_config(t, true);
	if (!r)
		r = ring_setup(t, true);
	if (r)
		return r;
	memset(h->buf[0], 0, RINGB);
	memset(h->buf[1], 0, RINGB);
	h->tx_played = 0;
	h->rx_next = 0;

	/* TX start: four descriptors (40 ms) handed over */
	rmw(h->port + A_CTRL, BIT(1), 0);
	writel(TX_CAUSES, h->port + A_CAUSE);
	rmw(h->glob + G_PERR_MASK, 0, BIT(0));
	rmw(h->port + A_MASK, 0, TX_CAUSES);
	for (i = 1; i <= 4; i++)
		writel(i, h->glob + G_RING(RING_TX) + R_PTR);
	h->tx_prod = 4;
	writel(BIT(0), h->glob + G_DMA_CAUSE);
	rmw(h->glob + G_DMA_MASK, 0, BIT(0));
	rmw(h->sys + S_PADS, 0, BIT(2));
	rmw(h->port + A_CHAN, CHAN_TX_EN, 0);
	poll_clear(t, h->port + A_CHAN, CHAN_TX_RUN, "TX stop");
	rmw(h->port + A_CHAN, 0, CHAN_TX_EN);

	/* RX start: all 16 descriptors are the hardware's */
	rmw(h->port + A_CTRL, BIT(1), 0);
	writel(RX_CAUSES, h->port + A_CAUSE);
	rmw(h->glob + G_PERR_MASK, 0, BIT(0));
	rmw(h->port + A_MASK, 0, RX_CAUSES);
	writel(BIT(16), h->glob + G_DMA_CAUSE);
	rmw(h->glob + G_DMA_MASK, 0, BIT(16));
	rmw(h->sys + S_PADS, 0, BIT(2));
	rmw(h->port + A_CHAN, CHAN_RX_EN, 0);
	poll_clear(t, h->port + A_CHAN, CHAN_RX_RUN, "RX stop");
	rmw(h->port + A_CHAN, 0, CHAN_RX_EN);
	h->running = true;
	return 0;
}

static void audio_stop(struct tel *t)
{
	struct tel_hw *h = t->hw;

	/* RX stop, close */
	rmw(h->port + A_MASK, RX_CAUSES, 0);
	rmw(h->port + A_CTRL, BIT(1), 0);
	rmw(h->glob + G_DMA_MASK, BIT(16), 0);
	rmw(h->port + A_CHAN, CHAN_RX_EN, 0);
	poll_clear(t, h->port + A_CHAN, CHAN_RX_RUN, "RX stop");
	writel(BIT(16), h->glob + G_DMA_CAUSE);
	writel(0, h->glob + G_RING(RING_RX) + R_THRESH);
	ring_enable(t, RING_RX, false);
	rmw(h->glob + G_RING_VALID, BIT(RING_RX), 0);
	/* TX stop, close */
	rmw(h->port + A_MASK, TX_CAUSES, 0);
	rmw(h->glob + G_DMA_MASK, BIT(0), 0);
	rmw(h->port + A_CHAN, CHAN_TX_EN, 0);
	poll_clear(t, h->port + A_CHAN, CHAN_TX_RUN, "TX stop");
	writel(BIT(0), h->glob + G_DMA_CAUSE);
	writel(0, h->glob + G_RING(RING_TX) + R_THRESH);
	rmw(h->glob + G_PERR_MASK, BIT(0), 0);
	ring_enable(t, RING_TX, false);
	rmw(h->glob + G_RING_VALID, BIT(RING_TX), 0);
	writel(ALL_CAUSES, h->port + A_CAUSE);
	h->running = false;
}

/*
 * A descriptor as the stock audioDmaRingAttachBuffer (c02b501c) writes it,
 * TX before every hand-over (audioDmaRingTx c02b5204 attaches, then moves
 * PTR) and every descriptor at setup. The hardware writes status back
 * into w4 (and the time into w1, w0 bit 22) when it completes one
 * (audioDmaRingRx c02b4cc0 and audioDmaRingTxDone c02b4ed0 read them), so
 * a descriptor is never handed back as it came.
 */
static void desc_fill(struct tel_hw *h, int dir, unsigned int i)
{
	u32 *q = h->desc[dir] + 8 * i;

	q[0] = DESC_W0;
	q[1] = 0;
	q[2] = h->buf_dma[dir] + i * SEG;
	q[3] = 0;
	q[4] = 0;
	q[5] = (u32)(uintptr_t)(h->buf[dir] + i * SEG);
	q[6] = 0;
	q[7] = 0;
}

/*
 * An RX descriptor given back: what audioDmaRingRxAckRefillQuick (c02b4fc0)
 * rewrites, w5, w2 and w4 (status cleared); w0 is left alone there.
 */
static void desc_refill_rx(struct tel_hw *h, unsigned int i)
{
	u32 *q = h->desc[1] + 8 * i;

	q[5] = (u32)(uintptr_t)(h->buf[1] + i * SEG);
	q[2] = h->buf_dma[1] + i * SEG;
	q[4] = 0;
}

static irqreturn_t tel_irq(int irq, void *dev_id)
{
	struct tel *t = dev_id;
	struct tel_hw *h = t->hw;
	void __iomem *txr = h->glob + G_RING(RING_TX);
	void __iomem *rxr = h->glob + G_RING(RING_RX);
	u32 g, d, n, c;
	irqreturn_t ret = IRQ_NONE;

	spin_lock(&t->lock);
	t->st.irqs++;
	g = readl(h->glob + G_CAUSE) & readl(h->glob + G_MASK);
	if (g & G_ERR_SUM) {
		c = readl(h->port + A_CAUSE) & readl(h->port + A_MASK);
		writel(c, h->port + A_CAUSE);
		if (c) {
			t->st.hw_errors++;
			dev_err_ratelimited(t->dev, "audio port error %08x\n", c);
		}
		if (t->st.hw_errors >= 1000 && !h->err_masked) {
			/* a cause that comes back at once: stop listening */
			rmw(h->glob + G_PERR_MASK, BIT(0), 0);
			h->err_masked = true;
			dev_err(t->dev, "audio port errors masked after 1000\n");
		}
		ret = IRQ_HANDLED;
	}
	d = readl(h->glob + G_DMA_CAUSE) & readl(h->glob + G_DMA_MASK);
	if (d & (BIT(0) | BIT(16))) {
		/*
		 * 100 descriptors a second each way. Many more means the
		 * hardware does not wait for the highway (wrong descriptors,
		 * a port in error): stop feeding it rather than spin here.
		 * Counted from the completions of the rings that interrupted
		 * (a ring's DONE also holds ones another interrupt will take).
		 */
		if (time_after(jiffies, h->win + HZ / 2)) {
			h->win = jiffies;
			h->win_n = 0;
		}
		if (d & BIT(0))
			h->win_n += min_t(u32, readl(txr + R_DONE) & 0x7ff, NDESC);
		if (d & BIT(16))
			h->win_n += min_t(u32, readl(rxr + R_DONE) & 0x7ff, NDESC);
		/*
		 * Not in the first second after taking the rings over: they
		 * come back from a halt with a backlog (TDM-NOTES.md).
		 */
		if (h->win_n > 300 && !h->unpaced &&
		    time_after(jiffies, h->settle)) {
			rmw(h->glob + G_DMA_MASK, BIT(0) | BIT(16), 0);
			h->unpaced = true;
			t->st.hw_errors++;
			dev_err(t->dev, "DMA unpaced (%u descriptors in 0.5 s): tx ptr %08x done %08x rx ptr %08x done %08x chan %08x cause %08x; DMA interrupts masked\n",
				h->win_n, readl(txr + R_PTR), readl(txr + R_DONE),
				readl(rxr + R_PTR), readl(rxr + R_DONE),
				readl(h->port + A_CHAN), readl(h->port + A_CAUSE));
			spin_unlock(&t->lock);
			return IRQ_HANDLED;
		}
	}
	if (d & BIT(0)) {
		/* TX descriptors played: hand the next ones over, fill one */
		writel(BIT(0), h->glob + G_DMA_CAUSE);
		n = min_t(u32, readl(txr + R_DONE) & 0x7ff, NDESC);
		while (n--) {
			unsigned int lead = clamp(tx_lead, 1u, 3u);
			unsigned int k;

			writel(0x8000 | 1, txr + R_DONE);
			/*
			 * descriptor tx_played is the one playing now; the
			 * hardware has it and the next three
			 */
			h->tx_played++;
			k = (h->tx_played + lead) % NDESC;
			desc_fill(h, 0, h->tx_prod);
			wmb();	/* the descriptor before its hand-over */
			h->tx_prod = (h->tx_prod + 1) % NDESC;
			writel(h->tx_prod, txr + R_PTR);
			tel_tx_block(t, h->buf[0] + k * SEG);
		}
		ret = IRQ_HANDLED;
	}
	if (d & BIT(16)) {
		/* RX descriptors filled: take them, give them back */
		writel(BIT(16), h->glob + G_DMA_CAUSE);
		n = min_t(u32, readl(rxr + R_DONE) & 0x7ff, NDESC);
		for (c = 0; c < n; c++) {
			tel_rx_block(t, h->buf[1] + h->rx_next * SEG);
			desc_refill_rx(h, h->rx_next);
			h->rx_next = (h->rx_next + 1) % NDESC;
		}
		if (n) {
			wmb();	/* the descriptors before they are given back */
			writel(((readl(rxr + R_PTR) & 0x7ff) + n) % NDESC,
			       rxr + R_PTR);
			writel(0x8000 | n, rxr + R_DONE);
		}
		ret = IRQ_HANDLED;
	}
	spin_unlock(&t->lock);
	return ret;
}

static const struct {
	const char *name;
	unsigned int off;	/* 0x0xxx port, 0x1xxx global */
} tel_reg_list[] = {
	{ "ctrl", 0x000 }, { "clk", 0x004 }, { "cause", 0x008 },
	{ "mask", 0x00c }, { "chan", 0x010 }, { "tx_fmt", 0x108 },
	{ "tx_tdm", 0x10c }, { "rx_fmt", 0x208 }, { "rx_tdm", 0x20c },
	{ "g_id", 0x1400 }, { "g_cause", 0x1410 }, { "g_mask", 0x1414 },
	{ "perr_cause", 0x1418 }, { "perr_mask", 0x141c },
	{ "dma_cause", 0x1438 }, { "dma_mask", 0x143c },
	{ "route", 0x1500 }, { "dma_cfg", 0x1800 }, { "ring_valid", 0x1804 },
	{ "ring_st", 0x1808 }, { "tx_cfg", 0x1880 }, { "tx_ptr", 0x1884 },
	{ "tx_done", 0x1888 }, { "tx_base", 0x188c }, { "rx_cfg", 0x18a0 },
	{ "rx_ptr", 0x18a4 }, { "rx_done", 0x18a8 }, { "rx_base", 0x18ac },
};

static u32 tel_reg(struct tel *t, unsigned int off)
{
	return readl((off & 0x1000 ? t->hw->glob : t->hw->port) + (off & 0xfff));
}

static ssize_t tel_hw_regs(struct tel *t, char *buf)
{
	int i, n = 0;

	for (i = 0; i < ARRAY_SIZE(tel_reg_list); i++)
		n += sysfs_emit_at(buf, n, "%s %08x\n", tel_reg_list[i].name,
				   tel_reg(t, tel_reg_list[i].off));
	n += sysfs_emit_at(buf, n, "sys_clk %08x\nsys_pads %08x\n",
			   readl(t->hw->sys + S_CLK), readl(t->hw->sys + S_PADS));
	return n;
}

static void tel_hw_dump(struct tel *t, const char *when)
{
	dev_info(t->dev, "%s: ctrl %08x clk %08x cause %08x mask %08x chan %08x tx %08x/%08x rx %08x/%08x\n",
		 when, tel_reg(t, 0x000), tel_reg(t, 0x004), tel_reg(t, 0x008),
		 tel_reg(t, 0x00c), tel_reg(t, 0x010), tel_reg(t, 0x108),
		 tel_reg(t, 0x10c), tel_reg(t, 0x208), tel_reg(t, 0x20c));
	dev_info(t->dev, "%s: id %08x g %08x/%08x perr %08x/%08x dma %08x/%08x route %08x\n",
		 when, tel_reg(t, 0x1400), tel_reg(t, 0x1410), tel_reg(t, 0x1414),
		 tel_reg(t, 0x1418), tel_reg(t, 0x141c), tel_reg(t, 0x1438),
		 tel_reg(t, 0x143c), tel_reg(t, 0x1500));
	dev_info(t->dev, "%s: dma_cfg %08x valid %08x st %08x tx ring %08x %08x %08x %08x rx ring %08x %08x %08x %08x\n",
		 when, tel_reg(t, 0x1800), tel_reg(t, 0x1804), tel_reg(t, 0x1808),
		 tel_reg(t, 0x1880), tel_reg(t, 0x1884), tel_reg(t, 0x1888),
		 tel_reg(t, 0x188c), tel_reg(t, 0x18a0), tel_reg(t, 0x18a4),
		 tel_reg(t, 0x18a8), tel_reg(t, 0x18ac));
	dev_info(t->dev, "%s: sys strap %08x clk %08x pads %08x\n", when,
		 readl(t->hw->sys + S_STRAP), readl(t->hw->sys + S_CLK),
		 readl(t->hw->sys + S_PADS));
}

/*
 * The rings as the stock kernel left them running (a RAM boot from it):
 * the ring enable command (0x83......) is never acknowledged on this box,
 * so they cannot be turned off and set up again. Keep the hardware's ring
 * bases and indices, write our descriptors where it looks for them (in the
 * reserved memory the stock kernel had them in), and go on from its index.
 */
static int audio_adopt(struct tel *t, struct platform_device *pdev)
{
	struct device *dev = t->dev;
	struct tel_hw *h = t->hw;
	struct device_node *np;
	struct reserved_mem *rm;
	void *map;
	unsigned int hw_i[2], prod[2], done[2];
	int d, i;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	rm = np ? of_reserved_mem_lookup(np) : NULL;
	of_node_put(np);
	if (!rm) {
		dev_err(dev, "running rings, but no memory-region to find them in\n");
		return -ENODEV;
	}
	map = devm_memremap(dev, rm->base, rm->size, MEMREMAP_WC);
	if (IS_ERR(map))
		return PTR_ERR(map);
	for (d = 0; d < 2; d++) {
		void __iomem *rr = h->glob + G_RING(d);
		u32 base = readl(rr + R_BASE), ptr = readl(rr + R_PTR);

		if (base < rm->base || base + NDESC * 32 > rm->base + rm->size ||
		    (base & 31)) {
			dev_err(dev, "ring %d at %08x, outside %pa+%pa\n", d,
				base, &rm->base, &rm->size);
			return -EINVAL;
		}
		h->desc[d] = map + (base - rm->base);
		h->desc_dma[d] = base;
		prod[d] = ptr & 0x7ff;
		hw_i[d] = (ptr >> 16) & 0x7ff;
		done[d] = readl(rr + R_DONE) & 0x7ff;
		dev_info(dev, "%s ring: base %08x ptr %08x (hw %u, producer %u) done %08x cfg %08x\n",
			 d ? "rx" : "tx", base, ptr, hw_i[d], prod[d],
			 readl(rr + R_DONE), readl(rr + R_CFG));
	}

	/* no DMA interrupts while the rings change hands */
	rmw(h->glob + G_DMA_MASK, BIT(0) | BIT(16), 0);
	for (d = 0; d < 2; d++) {
		/* take whatever completed while nobody looked */
		if (done[d])
			writel(0x8000 | done[d], h->glob + G_RING(d) + R_DONE);
		for (i = 0; i < NDESC; i++)
			desc_fill(h, d, i);
	}
	memset(h->buf[0], 0, RINGB);
	memset(h->buf[1], 0, RINGB);
	wmb();	/* the descriptors before the indices that hand them over */

	writel(ALL_CAUSES, h->port + A_CAUSE);
	writel(BIT(0) | BIT(16), h->glob + G_DMA_CAUSE);
	if (adopt_restart) {
		/* the channel toggle of the stock Start, both ways */
		rmw(h->port + A_CHAN, CHAN_TX_EN | CHAN_RX_EN, 0);
		poll_clear(t, h->port + A_CHAN, CHAN_TX_RUN | CHAN_RX_RUN,
			   "channels stop");
		writel(ALL_CAUSES, h->port + A_CAUSE);
		rmw(h->port + A_CHAN, 0, CHAN_TX_EN | CHAN_RX_EN);
	}

	/*
	 * TX: PTR[10:0] is the producer; the hardware plays descriptors from
	 * its index up to it (stops when they are equal, the found 0x800c000c
	 * state). Four in flight from the hardware index, as the stock Start.
	 * RX: PTR[10:0] is the consumer, i.e. the hardware index minus the
	 * completions not yet taken: the stock refill (c02b7110..c02b7130)
	 * adds n to it as it takes n (DONE -= n, c02b713c), so it trails the
	 * hardware index by DONE. The ring is full at 15 (as found: hw 7, PTR
	 * 8, DONE 15). We took every completion above, so PTR = hw: fifteen
	 * free, as the running stock (PTR 0x00030003, DONE 0).
	 */
	h->tx_played = hw_i[0];
	h->tx_prod = (hw_i[0] + 4) % NDESC;
	h->rx_next = hw_i[1];
	h->adopted = true;

	rmw(h->glob + G_MASK, 0, 0xa3000000);
	rmw(h->glob + G_PERR_MASK, 0, BIT(0));
	rmw(h->port + A_MASK, 0, TX_CAUSES | RX_CAUSES);
	rmw(h->glob + G_DMA_MASK, 0, BIT(0) | BIT(16));
	writel(h->tx_prod, h->glob + G_RING(RING_TX) + R_PTR);
	h->win = jiffies;
	h->win_n = 0;
	h->settle = jiffies + HZ;
	writel(hw_i[1], h->glob + G_RING(RING_RX) + R_PTR);
	h->running = true;
	return 0;
}

/* the ring status command, which this box does answer */
static int ring_status(struct tel *t, u32 *mask)
{
	void __iomem *cmd = t->hw->glob + G_RING_CMD;

	writel(0x82100000, cmd);
	if (poll_clear(t, cmd, BIT(31), "ring status"))
		return -ETIMEDOUT;
	*mask = readl(cmd) & 0xffff;
	return 0;
}

static int tel_hw_probe(struct tel *t, struct platform_device *pdev)
{
	struct device *dev = t->dev;
	struct tel_hw *h;
	struct resource *res;
	u32 rings;
	int r, i;

	h = devm_kzalloc(dev, sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	t->hw = h;
	h->port = devm_platform_ioremap_resource_byname(pdev, "port");
	if (IS_ERR(h->port))
		return PTR_ERR(h->port);
	h->glob = devm_platform_ioremap_resource_byname(pdev, "global");
	if (IS_ERR(h->glob))
		return PTR_ERR(h->glob);
	/* shared with other drivers (clocks, pins): mapped, not claimed */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sys");
	if (!res)
		return -ENODEV;
	h->sys = devm_ioremap(dev, res->start, resource_size(res));
	if (!h->sys)
		return -ENOMEM;
	h->irq = platform_get_irq(pdev, 0);
	if (h->irq < 0)
		return h->irq;

	tel_hw_dump(t, "as found");

	r = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (r)
		return r;
	for (i = 0; i < 2; i++) {
		h->buf[i] = dmam_alloc_coherent(dev, RINGB, &h->buf_dma[i],
						GFP_KERNEL);
		if (!h->buf[i])
			return -ENOMEM;
	}

	r = ring_status(t, &rings);
	if (r)
		return r;
	if ((rings & (BIT(RING_TX) | BIT(RING_RX))) ==
	    (BIT(RING_TX) | BIT(RING_RX))) {
		dev_info(dev, "rings %04x running: taking them over\n", rings);
		/* nothing from the old owner's interrupts until it is done */
		rmw(h->glob + G_DMA_MASK, BIT(0) | BIT(16), 0);
		rmw(h->glob + G_PERR_MASK, BIT(0), 0);
		r = devm_request_irq(dev, h->irq, tel_irq, 0, "ffxav-tdm", t);
		if (r)
			return r;
		r = audio_adopt(t, pdev);
		tel_hw_dump(t, r ? "adopt failed" : "adopted");
		if (r)
			return r;
		dev_info(dev, "TDM: adopted, buffers at %pad (tx) %pad (rx), descriptors at %pad %pad\n",
			 &h->buf_dma[0], &h->buf_dma[1], &h->desc_dma[0],
			 &h->desc_dma[1]);
		return 0;
	}

	/* a cold start (not tried: the ring enable command never answers) */
	for (i = 0; i < 2; i++) {
		h->desc[i] = dmam_alloc_coherent(dev, NDESC * 32,
						 &h->desc_dma[i], GFP_KERNEL);
		if (!h->desc[i])
			return -ENOMEM;
	}
	r = audio_init(t);
	if (r)
		return r;
	r = devm_request_irq(dev, h->irq, tel_irq, 0, "ffxav-tdm", t);
	if (r)
		return r;
	/* the board's module starts the highway at load and never stops it */
	r = audio_start(t);
	tel_hw_dump(t, r ? "start failed" : "started");
	if (r)
		return r;
	dev_info(dev, "TDM: 8 kHz, 8 slots of 32 bits, rings at %pad (tx) %pad (rx)\n",
		 &h->buf_dma[0], &h->buf_dma[1]);
	return 0;
}

static void tel_hw_remove(struct tel *t)
{
	struct tel_hw *h = t->hw;

	if (!h || !h->running)
		return;
	if (h->adopted) {
		/* the rings cannot be turned off here: just stop feeding them */
		rmw(h->glob + G_DMA_MASK, BIT(0) | BIT(16), 0);
		h->running = false;
		return;
	}
	audio_stop(t);
}

/* ---- statistics: /sys/devices/.../stats ---- */

static ssize_t stats_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct tel *t = dev_get_drvdata(dev);
	unsigned long slic_rd = 0, slic_wr = 0, slic_err = 0;
	int n;

	mutex_lock(&slic_mutex);
	if (the_slic) {
		slic_rd = the_slic->reads;
		slic_wr = the_slic->writes;
		slic_err = the_slic->errors;
	}
	mutex_unlock(&slic_mutex);
	spin_lock_irq(&t->lock);
	n = sysfs_emit(buf,
		"irqs %lu\nframes %lu\nblocks %lu\nreads %lu\nwrites %lu\n"
		"rx_overruns %lu\ntx_underruns %lu\ntx_overwrites %lu\n"
		"hw_errors %lu\nrx_queued %u\ntx_queued %u\ntx_credit %u\n"
		"rx_on %d%d\ntx_on %d%d\nslic %s\nslic_reads %lu\n"
		"slic_writes %lu\nslic_errors %lu\n",
		t->st.irqs, t->st.frames, t->st.blocks, t->st.reads,
		t->st.writes, t->st.rx_overruns, t->st.tx_underruns,
		t->st.tx_overwrites, t->st.hw_errors, t->rx_n, t->tx_n,
		t->tx_credit, t->rx_on[0], t->rx_on[1], t->tx_on[0],
		t->tx_on[1], the_slic ? "bound" : "none", slic_rd, slic_wr,
		slic_err);
	n += sysfs_emit_at(buf, n, "adopted %d\nunpaced %d\nerrors_masked %d\n",
			   t->hw->adopted, t->hw->unpaced, t->hw->err_masked);
	spin_unlock_irq(&t->lock);
	return n;
}
static DEVICE_ATTR_RO(stats);

static ssize_t regs_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return tel_hw_regs(dev_get_drvdata(dev), buf);
}
static DEVICE_ATTR_RO(regs);

static struct attribute *tel_attrs[] = {
	&dev_attr_stats.attr,
	&dev_attr_regs.attr,
	NULL
};
ATTRIBUTE_GROUPS(tel);

/* ---- the platform device ---- */

static int tel_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tel *t;
	int r, c;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->dev = dev;
	spin_lock_init(&t->lock);
	init_waitqueue_head(&t->wq);
	mutex_init(&t->open_lock);
	platform_set_drvdata(pdev, t);

	/* held in reset from here until the application lets it go */
	t->slic_reset = devm_gpiod_get_optional(dev, "slic-reset",
						GPIOD_OUT_HIGH);
	if (IS_ERR(t->slic_reset))
		return dev_err_probe(dev, PTR_ERR(t->slic_reset),
				     "SLIC reset line\n");
	if (!t->slic_reset)
		dev_warn(dev, "no slic-reset-gpios: ioctl 0x71b6 fails\n");

	r = tel_hw_probe(t, pdev);
	if (r)
		return r;

	for (c = 0; c < CHANS; c++) {
		struct tel_fxs *x = &t->fxs[c];

		x->t = t;
		x->ch = c;
		snprintf(x->name, sizeof(x->name), "fxs%d", c);
		x->misc.minor = MISC_DYNAMIC_MINOR;
		x->misc.name = x->name;
		x->misc.fops = &fxs_fops;
		x->misc.parent = dev;
		r = misc_register(&x->misc);
		if (r)
			goto fail;
	}
	t->pcm.minor = MISC_DYNAMIC_MINOR;
	t->pcm.name = "pcm";
	t->pcm.fops = &pcm_fops;
	t->pcm.parent = dev;
	r = misc_register(&t->pcm);
	if (r)
		goto fail;
	return 0;
fail:
	while (c-- > 0)
		misc_deregister(&t->fxs[c].misc);
	tel_hw_remove(t);
	return r;
}

static void tel_remove(struct platform_device *pdev)
{
	struct tel *t = platform_get_drvdata(pdev);
	int c;

	misc_deregister(&t->pcm);
	for (c = 0; c < CHANS; c++)
		misc_deregister(&t->fxs[c].misc);
	tel_hw_remove(t);
}

static const struct of_device_id tel_of_match[] = {
	{ .compatible = "marvell,ffxav-tdm" },
	{ }
};
MODULE_DEVICE_TABLE(of, tel_of_match);

static struct platform_driver tel_driver = {
	.probe = tel_probe,
	.remove_new = tel_remove,
	.driver = {
		.name = "ffxav-tdm",
		.of_match_table = tel_of_match,
		.dev_groups = tel_groups,
	},
};

static int __init tel_init(void)
{
	int r = spi_register_driver(&tel_slic_driver);

	if (r)
		return r;
	r = platform_driver_register(&tel_driver);
	if (r)
		spi_unregister_driver(&tel_slic_driver);
	return r;
}
module_init(tel_init);

static void __exit tel_exit(void)
{
	platform_driver_unregister(&tel_driver);
	spi_unregister_driver(&tel_slic_driver);
}
module_exit(tel_exit);

MODULE_DESCRIPTION("OBi200 telephony: FXS and PCM devices of the FFxAV TDM block and Si3226x SLIC");
MODULE_LICENSE("GPL");
