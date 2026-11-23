// SPDX-License-Identifier: GPL-2.0
/*
 * Watchdog of the Marvell FFxAV (88E7251), as on the Obihai OBi200, at
 * 0x8000d800. It looks like a DesignWare APB watchdog (the restart key
 * 0x76, the interrupt cleared by a read of 0x14); what we use of it:
 *
 *   0x00  CTRL    7 as the board's own firmware writes it: bit 0 enable,
 *                 bit 1 interrupt first (the reset comes at the second
 *                 expiry), bit 2 reset pulse length
 *   0x04  PERIOD  bits 3:0 the period, 0.1 s x (n + 1), so 0xff (the
 *                 maker's upper nibble 0xf kept) is 1.6 s
 *   0x08  the counter (read only here, for the dump)
 *   0x0c  FEED    write 0x76 to restart the count
 *   0x10  status (read only here, for the dump)
 *   0x14  ACK     a read clears the interrupt of the first expiry
 *
 * The board's firmware (ffxav_wdt.ko) writes 0xbfffffff to the system
 * register at 0x8000f028 (bit 30 cleared: the routing of the watchdog's
 * reset, as far as we can tell) before it starts the dog with PERIOD 0xf4
 * and CTRL 7, and feeds it from the dog's own interrupt (IRQ 3). We do the
 * same set-up, but with the longest period, and leave the feeding to the
 * watchdog core: the hardware heartbeat is 1.6 s, the core feeds it
 * from the kernel until user space opens /dev/watchdog and then for as
 * long as user space keeps up with the timeout it asked for (any length).
 * There is no stop: once started, the dog is fed by somebody or the box
 * resets.
 *
 * Until this driver binds, the tick feeds the dog
 * (drivers/clocksource/timer-ffxav.c); it stops when we take over.
 * Restart is in arch/arm/mach-ffxav/ffxav.c.
 *
 * Written for the v90d project from the board's own driver
 * (SPI-LED-WDT-NOTES.md); no vendor code.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/marvell/ffxav.h>
#include <linux/watchdog.h>

#define WDT_CTRL	0x00
#define WDT_PERIOD	0x04
#define WDT_COUNT	0x08
#define WDT_FEED	0x0c
#define WDT_STAT	0x10
#define WDT_ACK		0x14

#define WDT_CTRL_RUN	0x7		/* enable, interrupt first, pulse */
#define WDT_PERIOD_MAX	0xff		/* 1.6 s */
#define WDT_KEY		0x76
#define WDT_HW_MS	1600

#define SYS_WDT_ROUTE	0xbfffffff	/* as the maker's module writes it */

struct ffxav_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
};

static int ffxav_wdt_ping(struct watchdog_device *wdd)
{
	struct ffxav_wdt *w = watchdog_get_drvdata(wdd);

	writel(WDT_KEY, w->base + WDT_FEED);
	(void)readl(w->base + WDT_ACK);
	return 0;
}

static int ffxav_wdt_start(struct watchdog_device *wdd)
{
	struct ffxav_wdt *w = watchdog_get_drvdata(wdd);

	writel(WDT_PERIOD_MAX, w->base + WDT_PERIOD);
	writel(WDT_CTRL_RUN, w->base + WDT_CTRL);
	return ffxav_wdt_ping(wdd);
}

static const struct watchdog_info ffxav_wdt_info = {
	.options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE,
	.identity = "FFxAV watchdog",
};

static const struct watchdog_ops ffxav_wdt_ops = {
	.owner = THIS_MODULE,
	.start = ffxav_wdt_start,
	.ping = ffxav_wdt_ping,
};

static int ffxav_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *sys = NULL;
	struct ffxav_wdt *w;
	int ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w->base))
		return PTR_ERR(w->base);

	/* the system register: not requested, other code uses it too */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		sys = devm_ioremap(dev, res->start, resource_size(res));
		if (!sys)
			return -ENOMEM;
	}

	dev_info(dev, "ctrl %08x period %08x count %08x stat %08x sys %08x\n",
		 readl(w->base + WDT_CTRL), readl(w->base + WDT_PERIOD),
		 readl(w->base + WDT_COUNT), readl(w->base + WDT_STAT),
		 sys ? readl(sys) : 0);

	if (sys)
		writel(SYS_WDT_ROUTE, sys);

	w->wdd.info = &ffxav_wdt_info;
	w->wdd.ops = &ffxav_wdt_ops;
	w->wdd.parent = dev;
	w->wdd.min_timeout = 1;
	w->wdd.max_hw_heartbeat_ms = WDT_HW_MS;
	w->wdd.timeout = 60;
	watchdog_set_drvdata(&w->wdd, w);
	watchdog_init_timeout(&w->wdd, 0, dev);

	/* running from here on, whatever it was before */
	ffxav_wdt_start(&w->wdd);
	set_bit(WDOG_HW_RUNNING, &w->wdd.status);

	ret = devm_watchdog_register_device(dev, &w->wdd);
	if (ret)
		return ret;	/* the tick still feeds it, for ffxav_feed_s */

	/* the core feeds it from now on (at once, then every 0.8 s) */
	ffxav_timer_stop_feeding();
	dev_info(dev, "running, %u s timeout for user space; now ctrl %08x period %08x sys %08x\n",
		 w->wdd.timeout, readl(w->base + WDT_CTRL),
		 readl(w->base + WDT_PERIOD), sys ? readl(sys) : 0);
	return 0;
}

static const struct of_device_id ffxav_wdt_of_match[] = {
	{ .compatible = "marvell,ffxav-wdt" },
	{ }
};
MODULE_DEVICE_TABLE(of, ffxav_wdt_of_match);

static struct platform_driver ffxav_wdt_driver = {
	.probe = ffxav_wdt_probe,
	.driver = {
		.name = "ffxav-wdt",
		.of_match_table = ffxav_wdt_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(ffxav_wdt_driver);

MODULE_DESCRIPTION("Marvell FFxAV watchdog");
MODULE_LICENSE("GPL");
