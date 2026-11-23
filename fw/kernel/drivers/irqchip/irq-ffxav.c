// SPDX-License-Identifier: GPL-2.0
/*
 * Interrupt controller of the Marvell FFxAV (88E7251), as on the OBi200.
 *
 * 32 sources, all level: STATUS (0x00) shows the enabled sources that are
 * active, a write of 1s to ENABLE (0x08) enables (it reads back the enable
 * mask), to DISABLE (0x0c) disables. Nothing to acknowledge here: each
 * device clears its own request. The lowest pending source is served first,
 * as the board's own kernel does.
 *
 * Written for the v90d project from the controller as the board's own
 * kernel drives it (fw/obi/README.md).
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <asm/exception.h>

#define INTC_NR		32
#define INTC_STATUS	0x00
#define INTC_ENABLE	0x08
#define INTC_DISABLE	0x0c

static void __iomem *intc_base;
static struct irq_domain *intc_domain;

static void intc_mask(struct irq_data *d)
{
	writel(BIT(d->hwirq), intc_base + INTC_DISABLE);
}

static void intc_unmask(struct irq_data *d)
{
	writel(BIT(d->hwirq), intc_base + INTC_ENABLE);
}

static struct irq_chip intc_chip = {
	.name		= "ffxav",
	.irq_mask	= intc_mask,
	.irq_unmask	= intc_unmask,
};

static int intc_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &intc_chip, handle_level_irq);
	irq_set_status_flags(irq, IRQ_LEVEL);
	irq_set_probe(irq);
	return 0;
}

static const struct irq_domain_ops intc_ops = {
	.map	= intc_map,
	.xlate	= irq_domain_xlate_onecell,
};

static void __exception_irq_entry intc_handle_irq(struct pt_regs *regs)
{
	u32 pending;

	while ((pending = readl(intc_base + INTC_STATUS)) != 0)
		generic_handle_domain_irq(intc_domain, __ffs(pending));
}

static int __init intc_of_init(struct device_node *node, struct device_node *parent)
{
	intc_base = of_iomap(node, 0);
	if (!intc_base)
		return -ENOMEM;
	writel(~0u, intc_base + INTC_DISABLE);		/* everything off */
	intc_domain = irq_domain_add_linear(node, INTC_NR, &intc_ops, NULL);
	if (!intc_domain) {
		iounmap(intc_base);
		return -ENOMEM;
	}
	set_handle_irq(intc_handle_irq);
	return 0;
}
IRQCHIP_DECLARE(ffxav_intc, "marvell,ffxav-intc", intc_of_init);
