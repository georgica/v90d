// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO banks of the DSPG DVF99 SoC. Five banks of 32 pins; every setting has
 * a read-back register and separate set and clear registers, so nothing
 * needs a lock. Requesting a line takes the pad away from its peripheral
 * function. The 16 pads that can interrupt are not handled here.
 *
 * Written for the v90d project from refs/dvf99-io-facts.md section 4.
 */
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define BANK(n)		((n) / 32 * 0x60)
#define PIN(n)		BIT((n) % 32)
#define G_DATA		0x00
#define G_SET		0x04
#define G_CLR		0x08
#define G_DIR		0x0c	/* 1 = input */
#define G_DIR_OUT	0x10
#define G_DIR_IN	0x14
#define G_EN_SET	0x1c	/* 1 = the pad is a GPIO */

struct dvf99_gpio {
	struct gpio_chip gc;
	void __iomem *base;
};

static int dvf99_gpio_request(struct gpio_chip *gc, unsigned int off)
{
	struct dvf99_gpio *g = gpiochip_get_data(gc);

	writel(PIN(off), g->base + BANK(off) + G_EN_SET);
	return 0;
}

static int dvf99_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	struct dvf99_gpio *g = gpiochip_get_data(gc);

	return readl(g->base + BANK(off) + G_DIR) & PIN(off) ?
		GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int dvf99_gpio_direction_input(struct gpio_chip *gc, unsigned int off)
{
	struct dvf99_gpio *g = gpiochip_get_data(gc);

	writel(PIN(off), g->base + BANK(off) + G_DIR_IN);
	return 0;
}

static void dvf99_gpio_set(struct gpio_chip *gc, unsigned int off, int value)
{
	struct dvf99_gpio *g = gpiochip_get_data(gc);

	writel(PIN(off), g->base + BANK(off) + (value ? G_SET : G_CLR));
}

static int dvf99_gpio_direction_output(struct gpio_chip *gc, unsigned int off, int value)
{
	struct dvf99_gpio *g = gpiochip_get_data(gc);

	dvf99_gpio_set(gc, off, value);
	writel(PIN(off), g->base + BANK(off) + G_DIR_OUT);
	return 0;
}

static int dvf99_gpio_get(struct gpio_chip *gc, unsigned int off)
{
	struct dvf99_gpio *g = gpiochip_get_data(gc);

	return !!(readl(g->base + BANK(off) + G_DATA) & PIN(off));
}

static int dvf99_gpio_probe(struct platform_device *pdev)
{
	struct dvf99_gpio *g;

	g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;
	g->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(g->base))
		return PTR_ERR(g->base);

	g->gc.label = "dvf99-gpio";
	g->gc.parent = &pdev->dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = 160;
	g->gc.request = dvf99_gpio_request;
	g->gc.get_direction = dvf99_gpio_get_direction;
	g->gc.direction_input = dvf99_gpio_direction_input;
	g->gc.direction_output = dvf99_gpio_direction_output;
	g->gc.get = dvf99_gpio_get;
	g->gc.set = dvf99_gpio_set;
	return devm_gpiochip_add_data(&pdev->dev, &g->gc, g);
}

static const struct of_device_id dvf99_gpio_of_match[] = {
	{ .compatible = "dspg,dvf99-gpio" },
	{ }
};

static struct platform_driver dvf99_gpio_driver = {
	.probe	= dvf99_gpio_probe,
	.driver	= {
		.name		= "dvf99-gpio",
		.of_match_table	= dvf99_gpio_of_match,
	},
};

static int __init dvf99_gpio_init(void)
{
	return platform_driver_register(&dvf99_gpio_driver);
}
subsys_initcall(dvf99_gpio_init);
