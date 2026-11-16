// SPDX-License-Identifier: GPL-2.0
/*
 * DVF99 clock and reset unit. The boot loader leaves the PLLs and the bus
 * clocks set up; the device tree describes them as fixed clocks. What is
 * left for the kernel is to open the gate and release the reset of each
 * block it uses, and the system reset.
 *
 * Written for the v90d project from refs/dvf99-core-facts.md section 4.
 */
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/platform_data/dvf99-cmu.h>

#define CMU_SWSYSRST	0x40
#define CMU_RESET(w)	(0x50 + 4 * (w))	/* 1 = held in reset */
#define CMU_GATE(w)	(0x58 + 4 * (w))	/* 1 = clock running */

static void __iomem *cmu;
static DEFINE_RAW_SPINLOCK(cmu_lock);

static void __iomem *cmu_base(void)
{
	struct device_node *np;

	if (cmu)
		return cmu;
	np = of_find_compatible_node(NULL, NULL, "dspg,dvf99-cmu");
	if (np) {
		cmu = of_iomap(np, 0);
		of_node_put(np);
	}
	return cmu;
}

static int cmu_bit(unsigned int reg, unsigned int nr, bool set)
{
	void __iomem *base = cmu_base();
	unsigned long flags;
	u32 v;

	if (!base || nr > 63)
		return -ENODEV;
	reg += 4 * (nr >> 5);
	raw_spin_lock_irqsave(&cmu_lock, flags);
	v = readl(base + reg);
	if (set)
		v |= BIT(nr & 31);
	else
		v &= ~BIT(nr & 31);
	writel(v, base + reg);
	raw_spin_unlock_irqrestore(&cmu_lock, flags);
	return 0;
}

int dvf99_cmu_gate(unsigned int nr, bool on)
{
	return cmu_bit(CMU_GATE(0), nr, on);
}
EXPORT_SYMBOL_GPL(dvf99_cmu_gate);

int dvf99_cmu_reset(unsigned int nr, bool assert)
{
	return cmu_bit(CMU_RESET(0), nr, assert);
}
EXPORT_SYMBOL_GPL(dvf99_cmu_reset);

u32 dvf99_cmu_read(unsigned int reg)
{
	void __iomem *base = cmu_base();

	return base ? readl(base + reg) : 0;
}
EXPORT_SYMBOL_GPL(dvf99_cmu_read);

void dvf99_cmu_update(unsigned int reg, u32 mask, u32 val)
{
	void __iomem *base = cmu_base();
	unsigned long flags;

	if (!base)
		return;
	raw_spin_lock_irqsave(&cmu_lock, flags);
	writel((readl(base + reg) & ~mask) | (val & mask), base + reg);
	raw_spin_unlock_irqrestore(&cmu_lock, flags);
}
EXPORT_SYMBOL_GPL(dvf99_cmu_update);

void dvf99_restart(void)
{
	void __iomem *base = cmu_base();

	if (!base)
		return;
	writel(0x7496398b, base + CMU_SWSYSRST);
	writel(0xb5c9c674, base + CMU_SWSYSRST);
}

static int dvf99_restart_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	dvf99_restart();
	return NOTIFY_DONE;
}

static struct notifier_block dvf99_restart_nb = {
	.notifier_call	= dvf99_restart_notify,
	.priority	= 128,
};

static int __init dvf99_cmu_init(void)
{
	if (!cmu_base())
		return 0;
	return register_restart_handler(&dvf99_restart_nb);
}
arch_initcall(dvf99_cmu_init);
