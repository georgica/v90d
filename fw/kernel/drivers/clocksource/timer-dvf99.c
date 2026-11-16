// SPDX-License-Identifier: GPL-2.0
/*
 * Timers of the DSPG DVF99 SoC: 32-bit down counters in pairs.
 *
 * The first timer of the node is the tick (clock event), the second, 0x20
 * above it, runs free as clock source and sched_clock. Both count the bus
 * clock divided by 16.
 *
 * Written for the v90d project from refs/dvf99-core-facts.md section 2.
 */
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>
#include <linux/platform_data/dvf99-cmu.h>

#define TMR_LOAD	0x00
#define TMR_VALUE	0x04
#define TMR_CTRL	0x08
#define TMR_INTCLR	0x0c
#define TMR_SECOND	0x20		/* the second timer of the pair */

#define CTRL_ONESHOT	BIT(0)
#define CTRL_32BIT	BIT(1)
#define CTRL_DIV16	BIT(2)
#define CTRL_IE		BIT(5)
#define CTRL_PERIODIC	BIT(6)
#define CTRL_RUN	BIT(7)

static void __iomem *tick_base, *free_base;
static unsigned long tick_rate;

static u64 notrace dvf99_sched_read(void)
{
	return ~readl_relaxed(free_base + TMR_VALUE);
}

static int dvf99_shutdown(struct clock_event_device *evt)
{
	writel(0, tick_base + TMR_CTRL);
	writel(0, tick_base + TMR_INTCLR);
	return 0;
}

static int dvf99_set_periodic(struct clock_event_device *evt)
{
	writel(0, tick_base + TMR_CTRL);
	writel(DIV_ROUND_CLOSEST(tick_rate, HZ), tick_base + TMR_LOAD);
	writel(CTRL_32BIT | CTRL_DIV16 | CTRL_IE | CTRL_PERIODIC | CTRL_RUN, tick_base + TMR_CTRL);
	return 0;
}

static int dvf99_set_oneshot(struct clock_event_device *evt)
{
	writel(0, tick_base + TMR_CTRL);
	return 0;
}

static int dvf99_next_event(unsigned long delta, struct clock_event_device *evt)
{
	writel(0, tick_base + TMR_CTRL);
	writel(delta, tick_base + TMR_LOAD);
	writel(CTRL_32BIT | CTRL_DIV16 | CTRL_IE | CTRL_ONESHOT | CTRL_RUN, tick_base + TMR_CTRL);
	return 0;
}

static struct clock_event_device dvf99_clockevent = {
	.name			= "dvf99-timer1",
	.features		= CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 300,
	.set_state_shutdown	= dvf99_shutdown,
	.set_state_periodic	= dvf99_set_periodic,
	.set_state_oneshot	= dvf99_set_oneshot,
	.tick_resume		= dvf99_shutdown,
	.set_next_event		= dvf99_next_event,
};

static irqreturn_t dvf99_tick(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	writel(0, tick_base + TMR_INTCLR);
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

static int __init dvf99_timer_init(struct device_node *np)
{
	struct clk *clk;
	int irq, ret;

	tick_base = of_iomap(np, 0);
	if (!tick_base)
		return -ENOMEM;
	free_base = tick_base + TMR_SECOND;

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;
	tick_rate = clk_get_rate(clk) / 16;

	irq = irq_of_parse_and_map(np, 0);
	if (!irq)
		return -EINVAL;

	/* the boot loader runs the first timer only; the second shares its reset */
	dvf99_cmu_gate(DVF99_GATE_TIMER1, true);
	dvf99_cmu_gate(DVF99_GATE_TIMER2, true);

	writel(0, free_base + TMR_CTRL);
	writel(0xffffffff, free_base + TMR_LOAD);
	writel(CTRL_32BIT | CTRL_DIV16 | CTRL_PERIODIC | CTRL_RUN, free_base + TMR_CTRL);
	clocksource_mmio_init(free_base + TMR_VALUE, "dvf99-timer2", tick_rate, 200, 32,
			      clocksource_mmio_readl_down);
	sched_clock_register(dvf99_sched_read, 32, tick_rate);

	writel(0, tick_base + TMR_CTRL);
	writel(0, tick_base + TMR_INTCLR);
	ret = request_irq(irq, dvf99_tick, IRQF_TIMER | IRQF_IRQPOLL, "dvf99-timer1", &dvf99_clockevent);
	if (ret)
		return ret;
	dvf99_clockevent.cpumask = cpu_possible_mask;
	dvf99_clockevent.irq = irq;
	clockevents_config_and_register(&dvf99_clockevent, tick_rate, 16, 0xfffffff0);
	return 0;
}
TIMER_OF_DECLARE(dvf99_timer, "dspg,dvf99-timer", dvf99_timer_init);
