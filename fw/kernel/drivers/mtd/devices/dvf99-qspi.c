// SPDX-License-Identifier: GPL-2.0
/*
 * The serial flash of the Grandstream HT802 behind the DVF99 QSPI
 * controller, as an MTD device. All the work is in fw/drv/qspi.c (tested on
 * the host); this file is the kernel side of its five callbacks.
 *
 * The chip also holds the boot loaders and the stock firmware, the box's way
 * back without a serial cable. The fence is here: the core refuses to erase
 * or program outside the ranges listed in the node's "v90d,writable"
 * property, whatever MTD device the request came through (the device tree
 * says what the ranges are and why they cover both stock sets; the program
 * that writes, fw/tools/v90nv, narrows them to the set this box uses).
 *
 * Written for the v90d project; no vendor code.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_data/dvf99-cmu.h>
#include <linux/platform_device.h>

#include "qspi.h"

struct dvf99_qspi {
	struct mtd_info mtd;
	void __iomem *regs, *win;
	struct mutex lock;
	qspi_t q;
};

static uint32_t io_rd(void *ctx, unsigned int reg)
{
	struct dvf99_qspi *d = ctx;

	return readl(d->regs + reg);
}

static void io_wr(void *ctx, unsigned int reg, uint32_t v)
{
	struct dvf99_qspi *d = ctx;

	writel(v, d->regs + reg);
}

static void io_win_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t n)
{
	struct dvf99_qspi *d = ctx;

	memcpy_fromio(buf, d->win + off, n);
}

/* a page program: the words must reach the controller without a pause */
static void io_win_burst(void *ctx, uint32_t off, const uint32_t *w, unsigned int n)
{
	struct dvf99_qspi *d = ctx;
	void __iomem *p = d->win + off;
	unsigned long flags;
	unsigned int i;
	u32 sum = 0;

	for (i = 0; i < n; i++)		/* the data in the cache before we start */
		sum += w[i];
	local_irq_save(flags);
	for (i = 0; i < n; i++, p += 4)
		writel(w[i], p);
	local_irq_restore(flags);
	(void)sum;
}

static void io_udelay(void *ctx, unsigned int us)
{
	if (us >= 100)
		usleep_range(us, 2 * us);
	else
		udelay(us);
}

static int err_of(int r)
{
	switch (r) {
	case QSPI_OK:		return 0;
	case QSPI_E_RANGE:	return -EROFS;
	case QSPI_E_TIMEOUT:	return -ETIMEDOUT;
	default:		return -EIO;
	}
}

static int dvf99_qspi_read(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen, u_char *buf)
{
	struct dvf99_qspi *d = mtd->priv;
	int r;

	mutex_lock(&d->lock);
	r = qspi_read(&d->q, from, buf, len);
	mutex_unlock(&d->lock);
	*retlen = r ? 0 : len;
	return err_of(r);
}

static int dvf99_qspi_write(struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen, const u_char *buf)
{
	struct dvf99_qspi *d = mtd->priv;
	unsigned long before;
	int r;

	mutex_lock(&d->lock);
	before = d->q.retries;
	r = qspi_write(&d->q, to, buf, len);
	if (d->q.retries != before)
		dev_info_ratelimited(mtd->dev.parent, "page programs done again so far: %lu\n", d->q.retries);
	mutex_unlock(&d->lock);
	*retlen = r ? 0 : len;
	if (r)
		dev_err(mtd->dev.parent, "write of %zu at 0x%llx refused or failed: %d\n", len, to, r);
	return err_of(r);
}

static int dvf99_qspi_erase(struct mtd_info *mtd, struct erase_info *ei)
{
	struct dvf99_qspi *d = mtd->priv;
	int r;

	mutex_lock(&d->lock);
	r = qspi_erase(&d->q, ei->addr, ei->len);
	mutex_unlock(&d->lock);
	if (r) {
		ei->fail_addr = ei->addr;
		dev_err(mtd->dev.parent, "erase of 0x%llx at 0x%llx refused or failed: %d\n", ei->len, ei->addr, r);
	}
	return err_of(r);
}

/*
 * Reset 11 of the clock and reset unit is this controller. Found on the
 * unit: after a page program that the chip never received the controller
 * programs nothing more until it has been through this.
 */
#define DVF99_RESET_QSPI	11

static void io_reset(void *ctx)
{
	dvf99_cmu_reset(DVF99_RESET_QSPI, true);
	usleep_range(1000, 2000);
	dvf99_cmu_reset(DVF99_RESET_QSPI, false);
	usleep_range(1000, 2000);
}

static int dvf99_qspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dvf99_qspi *d;
	qspi_io io = { io_rd, io_wr, io_win_read, io_win_burst, io_udelay, io_reset, NULL };
	int i, n, r;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	mutex_init(&d->lock);
	d->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->regs))
		return PTR_ERR(d->regs);
	d->win = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(d->win))
		return PTR_ERR(d->win);

	io.ctx = d;
	r = qspi_init(&d->q, &io);
	if (r) {
		dev_err(dev, "flash not recognised (%d): left alone\n", r);
		return -ENODEV;
	}
	n = of_property_count_u32_elems(dev->of_node, "v90d,writable");
	for (i = 0; i + 1 < n; i += 2) {
		u32 lo, hi;

		of_property_read_u32_index(dev->of_node, "v90d,writable", i, &lo);
		of_property_read_u32_index(dev->of_node, "v90d,writable", i + 1, &hi);
		if (qspi_allow(&d->q, lo, hi))
			dev_warn(dev, "writable range 0x%x..0x%x not accepted\n", lo, hi);
		else
			dev_info(dev, "writable: 0x%06x..0x%06x\n", lo, hi);
	}

	d->mtd.name = "qspi";
	d->mtd.dev.parent = dev;
	mtd_set_of_node(&d->mtd, dev->of_node);
	d->mtd.priv = d;
	d->mtd.type = MTD_NORFLASH;
	d->mtd.flags = MTD_CAP_NORFLASH;
	d->mtd.size = QSPI_SIZE;
	d->mtd.erasesize = QSPI_SECTOR;
	d->mtd.writesize = 1;
	d->mtd.writebufsize = QSPI_PAGE;
	d->mtd._read = dvf99_qspi_read;
	d->mtd._write = dvf99_qspi_write;
	d->mtd._erase = dvf99_qspi_erase;
	platform_set_drvdata(pdev, d);
	{
		static const char *const names[] = {
			"unknown", "EON EN25QX128A", "Winbond W25Q128FV",
			"GigaDevice GD25Q128", "Micron N25Q128"
		};
		int c = d->q.chip;

		dev_info(dev, "%s 16 MiB, QPI\n",
			 (c >= 0 && c < (int)ARRAY_SIZE(names)) ? names[c] : "unknown");
	}
	return mtd_device_register(&d->mtd, NULL, 0);
}

static const struct of_device_id dvf99_qspi_of_match[] = {
	{ .compatible = "dspg,dvf99-qspi" },
	{ }
};

static struct platform_driver dvf99_qspi_driver = {
	.probe = dvf99_qspi_probe,
	.driver = {
		.name = "dvf99-qspi",
		.of_match_table = dvf99_qspi_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(dvf99_qspi_driver);

MODULE_DESCRIPTION("HT802 serial flash on the DVF99 QSPI controller");
MODULE_LICENSE("GPL");
