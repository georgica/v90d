// SPDX-License-Identifier: GPL-2.0
/*
 * The 16 general purpose pins of the Marvell 88E7251 ("FFxAV") as on the
 * Obihai OBi200, at 0x8000c000 (the board's own kernel: ffxav_gpio_*):
 *
 *   0x00  output levels, one bit per pin
 *   0x04  direction, a set bit drives the pin
 *   0x50  input levels
 *
 * The block also has interrupt registers (0x30..0x4c, demultiplexed on the
 * interrupt controller's line 12 by the board's kernel); nothing here uses
 * them. The board's kernel sets the level before it turns a pin to output,
 * as this driver does. On the OBi200 pin 0 is the SLIC's reset (low holds
 * it) and pin 2 its interrupt line (TDM-NOTES.md).
 *
 * Written for the v90d project from the board's kernel as it runs.
 */
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define GPIO_OUT	0x00
#define GPIO_DIR	0x04
#define GPIO_IN		0x50
#define GPIO_NR		16

struct ffxav_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	spinlock_t lock;	/* read-modify-write of OUT and DIR */
};

static void ffxav_gpio_rmw(struct ffxav_gpio *g, unsigned int reg,
			   unsigned int bit, int set)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&g->lock, flags);
	v = readl(g->base + reg);
	if (set)
		v |= BIT(bit);
	else
		v &= ~BIT(bit);
	writel(v, g->base + reg);
	spin_unlock_irqrestore(&g->lock, flags);
}

static int ffxav_gpio_get(struct gpio_chip *gc, unsigned int off)
{
	struct ffxav_gpio *g = gpiochip_get_data(gc);

	return !!(readl(g->base + GPIO_IN) & BIT(off));
}

static void ffxav_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	ffxav_gpio_rmw(gpiochip_get_data(gc), GPIO_OUT, off, val);
}

static int ffxav_gpio_dir_in(struct gpio_chip *gc, unsigned int off)
{
	ffxav_gpio_rmw(gpiochip_get_data(gc), GPIO_DIR, off, 0);
	return 0;
}

static int ffxav_gpio_dir_out(struct gpio_chip *gc, unsigned int off, int val)
{
	struct ffxav_gpio *g = gpiochip_get_data(gc);

	ffxav_gpio_rmw(g, GPIO_OUT, off, val);
	ffxav_gpio_rmw(g, GPIO_DIR, off, 1);
	return 0;
}

static int ffxav_gpio_get_dir(struct gpio_chip *gc, unsigned int off)
{
	struct ffxav_gpio *g = gpiochip_get_data(gc);

	return readl(g->base + GPIO_DIR) & BIT(off) ?
		GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int ffxav_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ffxav_gpio *g;
	struct resource *res;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	/*
	 * Mapped without claiming the range: the SPI controller of the same
	 * 4 KiB page (0x8000c800) is another device.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	g->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!g->base)
		return -ENOMEM;
	spin_lock_init(&g->lock);

	g->gc.label = "ffxav-gpio";
	g->gc.parent = dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = GPIO_NR;
	g->gc.get = ffxav_gpio_get;
	g->gc.set = ffxav_gpio_set;
	g->gc.direction_input = ffxav_gpio_dir_in;
	g->gc.direction_output = ffxav_gpio_dir_out;
	g->gc.get_direction = ffxav_gpio_get_dir;

	dev_info(dev, "out %04x dir %04x in %04x\n",
		 readl(g->base + GPIO_OUT) & 0xffff,
		 readl(g->base + GPIO_DIR) & 0xffff,
		 readl(g->base + GPIO_IN) & 0xffff);
	return devm_gpiochip_add_data(dev, &g->gc, g);
}

static const struct of_device_id ffxav_gpio_of_match[] = {
	{ .compatible = "marvell,ffxav-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, ffxav_gpio_of_match);

static struct platform_driver ffxav_gpio_driver = {
	.probe = ffxav_gpio_probe,
	.driver = {
		.name = "ffxav-gpio",
		.of_match_table = ffxav_gpio_of_match,
	},
};

static int __init ffxav_gpio_init(void)
{
	return platform_driver_register(&ffxav_gpio_driver);
}
subsys_initcall(ffxav_gpio_init);

MODULE_DESCRIPTION("Marvell FFxAV GPIO");
MODULE_LICENSE("GPL");
