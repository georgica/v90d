// SPDX-License-Identifier: GPL-2.0
/*
 * Watchdog of the DSPG DVF99 SoCs, as measured on a Grandstream HT802
 * (HARDWARE.md): three 16-bit registers,
 *   0x00  period: 0 = 1 s, 1 = 10 s, 2 = 100 s, 3 = 1000 s
 *   0x04  enable: 0xABCD written twice; reads 0 whether it runs or not
 *   0x08  refresh: 0xA5A5 then 0x5A5A
 * Once enabled it cannot be stopped, and we cannot ask whether the boot
 * loader (or a RAM boot from another system) started it. So it is started
 * here, at 100 s, and the watchdog core keeps it fed until user space
 * takes over and from then on for as long as user space asks in time.
 *
 * Written for the v90d project; no vendor code.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_data/dvf99-cmu.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define WDT_PERIOD	0x00
#define WDT_ENABLE	0x04
#define WDT_REFRESH	0x08

struct dvf99_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
	struct mutex lock;	/* one write sequence at a time */
};

/*
 * The block does not take writes that follow each other at CPU speed: a
 * first version of this driver, writing back to back, had the box reset a
 * few seconds after its probe, while the same values written milliseconds
 * apart from user space keep it alive. So every write is followed by a
 * pause. All callers may sleep.
 */
static void dvf99_wdt_write(struct dvf99_wdt *w, u16 v, unsigned int reg)
{
	writew(v, w->base + reg);
	usleep_range(2000, 3000);
}

static int dvf99_wdt_ping(struct watchdog_device *wdd)
{
	struct dvf99_wdt *w = watchdog_get_drvdata(wdd);

	mutex_lock(&w->lock);
	dvf99_wdt_write(w, 0xa5a5, WDT_REFRESH);
	dvf99_wdt_write(w, 0x5a5a, WDT_REFRESH);
	mutex_unlock(&w->lock);
	return 0;
}

static int dvf99_wdt_start(struct watchdog_device *wdd)
{
	struct dvf99_wdt *w = watchdog_get_drvdata(wdd);

	mutex_lock(&w->lock);
	dvf99_wdt_write(w, 2, WDT_PERIOD);
	dvf99_wdt_write(w, 0xabcd, WDT_ENABLE);
	dvf99_wdt_write(w, 0xabcd, WDT_ENABLE);
	mutex_unlock(&w->lock);
	return dvf99_wdt_ping(wdd);
}

static const struct watchdog_info dvf99_wdt_info = {
	.options = WDIOF_KEEPALIVEPING | WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE,
	.identity = "DVF99 watchdog",
};

static const struct watchdog_ops dvf99_wdt_ops = {
	.owner = THIS_MODULE,
	.start = dvf99_wdt_start,
	.ping = dvf99_wdt_ping,
};

static int dvf99_wdt_probe(struct platform_device *pdev)
{
	struct dvf99_wdt *w;

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w->base))
		return PTR_ERR(w->base);
	mutex_init(&w->lock);
	/* The slow (1 MHz) clock: on under the stock system; U-Boot sets its
	 * divider but not its enable (CMU clock switch register, bit 3). */
	dvf99_cmu_update(DVF99_CMU_CLKSW, BIT(3), BIT(3));
	w->wdd.info = &dvf99_wdt_info;
	w->wdd.ops = &dvf99_wdt_ops;
	w->wdd.parent = &pdev->dev;
	w->wdd.min_timeout = 1;
	w->wdd.max_hw_heartbeat_ms = 50000;	/* half of the 100 s period */
	w->wdd.timeout = 60;
	watchdog_set_drvdata(&w->wdd, w);
	watchdog_set_nowayout(&w->wdd, true);
	dvf99_wdt_start(&w->wdd);
	set_bit(WDOG_HW_RUNNING, &w->wdd.status);
	return devm_watchdog_register_device(&pdev->dev, &w->wdd);
}

static const struct of_device_id dvf99_wdt_of_match[] = {
	{ .compatible = "dspg,dvf99-wdt" },
	{ }
};

static struct platform_driver dvf99_wdt_driver = {
	.probe = dvf99_wdt_probe,
	.driver = {
		.name = "dvf99-wdt",
		.of_match_table = dvf99_wdt_of_match,
	},
};
builtin_platform_driver(dvf99_wdt_driver);

MODULE_DESCRIPTION("DSPG DVF99 watchdog");
MODULE_LICENSE("GPL");
