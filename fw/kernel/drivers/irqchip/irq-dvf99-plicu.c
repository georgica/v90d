// SPDX-License-Identifier: GPL-2.0
/*
 * Interrupt controller of the DSPG DVF99 SoC family ("PLICU").
 *
 * 64 sources in two 32-bit halves. A source is either level (active high)
 * or rising edge; edge sources latch and are acknowledged by a write. The
 * hardware names the enable registers "mask": writing SET enables.
 *
 * Written for the v90d project from the register facts in
 * refs/dvf99-core-facts.md.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>
#include <asm/exception.h>

#define PLICU_NR		64
#define PLICU_ACK(h)		(0x00 + 4 * (h))	/* write 1: clear an edge latch */
#define PLICU_EDGE(h)		(0x08 + 4 * (h))	/* 1 = rising edge, 0 = level high */
#define PLICU_CAUSE(h)		(0x2c + 4 * (h))	/* enabled and active */
#define PLICU_ENABLE(h)		(0x3c + 4 * (h))	/* write 1: enable */
#define PLICU_DISABLE(h)	(0x44 + 4 * (h))	/* write 1: disable */

static void __iomem *plicu_base;
static struct irq_domain *plicu_domain;
static DEFINE_RAW_SPINLOCK(plicu_lock);

static void plicu_mask(struct irq_data *d)
{
	writel(BIT(d->hwirq & 31), plicu_base + PLICU_DISABLE(d->hwirq >> 5));
}

static void plicu_unmask(struct irq_data *d)
{
	writel(BIT(d->hwirq & 31), plicu_base + PLICU_ENABLE(d->hwirq >> 5));
}

static void plicu_ack(struct irq_data *d)
{
	writel(BIT(d->hwirq & 31), plicu_base + PLICU_ACK(d->hwirq >> 5));
}

static int plicu_set_type(struct irq_data *d, unsigned int type)
{
	void __iomem *reg = plicu_base + PLICU_EDGE(d->hwirq >> 5);
	u32 bit = BIT(d->hwirq & 31), v;
	unsigned long flags;

	type &= IRQ_TYPE_SENSE_MASK;
	if (type != IRQ_TYPE_EDGE_RISING && type != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;

	raw_spin_lock_irqsave(&plicu_lock, flags);
	v = readl(reg);
	if (type == IRQ_TYPE_EDGE_RISING)
		v |= bit;
	else
		v &= ~bit;
	writel(v, reg);
	raw_spin_unlock_irqrestore(&plicu_lock, flags);

	if (type == IRQ_TYPE_EDGE_RISING) {
		irq_set_handler_locked(d, handle_edge_irq);
		plicu_ack(d);
	} else {
		irq_set_handler_locked(d, handle_level_irq);
	}
	return 0;
}

static struct irq_chip plicu_chip = {
	.name		= "plicu",
	.irq_mask	= plicu_mask,
	.irq_unmask	= plicu_unmask,
	.irq_ack	= plicu_ack,
	.irq_set_type	= plicu_set_type,
};

static int plicu_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &plicu_chip, handle_level_irq);
	irq_set_status_flags(irq, IRQ_LEVEL);
	irq_set_probe(irq);
	return 0;
}

static const struct irq_domain_ops plicu_ops = {
	.map	= plicu_map,
	.xlate	= irq_domain_xlate_onetwocell,
};

static void __exception_irq_entry plicu_handle_irq(struct pt_regs *regs)
{
	u32 cause;
	int half, again;

	do {
		again = 0;
		for (half = 0; half < 2; half++) {
			cause = readl(plicu_base + PLICU_CAUSE(half));
			while (cause) {
				int bit = __ffs(cause);

				cause &= ~BIT(bit);
				generic_handle_domain_irq(plicu_domain, half * 32 + bit);
				again = 1;
			}
		}
	} while (again);
}

static int __init plicu_of_init(struct device_node *node, struct device_node *parent)
{
	plicu_base = of_iomap(node, 0);
	if (!plicu_base)
		return -ENOMEM;

	/* everything off, level, and no stale latches */
	writel(~0u, plicu_base + PLICU_DISABLE(0));
	writel(~0u, plicu_base + PLICU_DISABLE(1));
	writel(0, plicu_base + PLICU_EDGE(0));
	writel(0, plicu_base + PLICU_EDGE(1));
	writel(~0u, plicu_base + PLICU_ACK(0));
	writel(~0u, plicu_base + PLICU_ACK(1));

	plicu_domain = irq_domain_add_linear(node, PLICU_NR, &plicu_ops, NULL);
	if (!plicu_domain) {
		iounmap(plicu_base);
		return -ENOMEM;
	}
	set_handle_irq(plicu_handle_irq);
	return 0;
}
IRQCHIP_DECLARE(dvf99_plicu, "dspg,dvf99-plicu", plicu_of_init);
