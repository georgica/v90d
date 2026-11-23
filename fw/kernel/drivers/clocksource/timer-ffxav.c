// SPDX-License-Identifier: GPL-2.0
/*
 * Timers of the Marvell FFxAV (88E7251), as on the OBi200: a block at
 * 0x8000d000 of 32-bit down-counters at 1 MHz.
 *
 *   timer 0  LOAD 0x00, VALUE 0x04, CTRL 0x08: free-running from ~0
 *            (CTRL 5), the clocksource and sched_clock
 *   timer 1  LOAD 0x14, CTRL 0x1c, ACK 0x20: the tick, periodic (CTRL 3)
 *            on its interrupt; reading ACK clears the request
 *
 * The watchdog (0x8000d800) is fed here too, from the tick, while nobody
 * else does: a write of 0x76 to 0x0c, and a read of 0x14 that clears its own
 * interrupt. Its period is 0.1 s x (PERIOD - 0xef), at most 1.6 s; the box
 * resets if it is not fed for about two periods. A kernel that stops ticking
 * is therefore reset, which during bring-up (fw/obi, kjump) brings the box
 * back to the firmware in its flash. A kernel that ticks but is stuck
 * would be kept alive for ever, so the tick feeds it for
 * ffxav_feed_s seconds only (60; 0 = for ever): after that the box resets.
 *
 * The watchdog driver (drivers/watchdog/ffxav_wdt.c) takes the feeding
 * over when it binds: it calls ffxav_timer_stop_feeding() and from then
 * on the watchdog core feeds the dog from its own worker, until user space
 * opens /dev/watchdog and keeps it fed. ffxav_feed_s is the fallback for a
 * kernel without that driver, or one where it does not bind.
 *
 * Written for the v90d project from the timers as the board's own kernel
 * drives them (fw/obi/README.md).
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <linux/soc/marvell/ffxav.h>

#define TMR_RATE	1000000

#define T0_LOAD		0x00
#define T0_VALUE	0x04
#define T0_CTRL		0x08
#define T1_LOAD		0x14
#define T1_CTRL		0x1c
#define T1_ACK		0x20

#define WDT_PERIOD	0x04
#define WDT_FEED	0x0c
#define WDT_ACK		0x14
#define WDT_KEY		0x76

static void __iomem *tmr;
static void __iomem *wdt;
static bool wdt_fed_here = true;
static unsigned int feed_s = 60;
core_param(ffxav_feed_s, feed_s, uint, 0444);
static unsigned long feed_ticks;

static u64 notrace ffxav_sched_read(void)
{
	return ~readl_relaxed(tmr + T0_VALUE);
}

static u64 ffxav_cs_read(struct clocksource *cs)
{
	return ~readl_relaxed(tmr + T0_VALUE);
}

static struct clocksource ffxav_cs = {
	.name	= "ffxav-t0",
	.rating	= 200,
	.read	= ffxav_cs_read,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

/* the watchdog driver takes over feeding (the watchdog core does it now) */
void ffxav_timer_stop_feeding(void)
{
	WRITE_ONCE(wdt_fed_here, false);
	pr_info("ffxav-timer: the watchdog driver feeds the watchdog now\n");
}
EXPORT_SYMBOL_GPL(ffxav_timer_stop_feeding);

static void ffxav_feed(void)
{
	if (feed_s && feed_ticks++ >= (unsigned long)feed_s * HZ)
		return;		/* not any more: the watchdog will reset the box */
	if (wdt && READ_ONCE(wdt_fed_here)) {
		writel_relaxed(WDT_KEY, wdt + WDT_FEED);
		(void)readl_relaxed(wdt + WDT_ACK);
	}
}

static int ffxav_shutdown(struct clock_event_device *ce)
{
	writel(0, tmr + T1_CTRL);
	return 0;
}

static int ffxav_periodic(struct clock_event_device *ce)
{
	writel(0, tmr + T1_CTRL);
	writel(DIV_ROUND_CLOSEST(TMR_RATE, HZ) - 1, tmr + T1_LOAD);
	writel(3, tmr + T1_CTRL);
	return 0;
}

static struct clock_event_device ffxav_ce = {
	.name			= "ffxav-t1",
	.features		= CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 200,
	.set_state_shutdown	= ffxav_shutdown,
	.set_state_periodic	= ffxav_periodic,
	.tick_resume		= ffxav_shutdown,
};

static irqreturn_t ffxav_tick(int irq, void *dev_id)
{
	(void)readl_relaxed(tmr + T1_ACK);
	ffxav_feed();
	ffxav_ce.event_handler(&ffxav_ce);
	return IRQ_HANDLED;
}

static int __init ffxav_timer_init(struct device_node *node)
{
	int irq, ret;

	tmr = of_iomap(node, 0);
	wdt = of_iomap(node, 1);	/* optional: the watchdog to feed */
	if (!tmr)
		return -ENOMEM;
	ffxav_feed();

	writel(0, tmr + T0_CTRL);
	writel(~0u, tmr + T0_LOAD);
	writel(5, tmr + T0_CTRL);
	sched_clock_register(ffxav_sched_read, 32, TMR_RATE);
	ret = clocksource_register_hz(&ffxav_cs, TMR_RATE);
	if (ret)
		return ret;

	irq = irq_of_parse_and_map(node, 0);
	if (!irq)
		return -EINVAL;
	writel(0, tmr + T1_CTRL);
	ret = request_irq(irq, ffxav_tick, IRQF_TIMER, "ffxav-tick", NULL);
	if (ret)
		return ret;
	ffxav_ce.irq = irq;
	ffxav_ce.cpumask = cpu_possible_mask;
	clockevents_config_and_register(&ffxav_ce, TMR_RATE, 1, 0xffffffff);
	return 0;
}
TIMER_OF_DECLARE(ffxav_timer, "marvell,ffxav-timer", ffxav_timer_init);
