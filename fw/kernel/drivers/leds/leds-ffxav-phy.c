// SPDX-License-Identifier: GPL-2.0
/*
 * LEDs of the Obihai OBi200 that hang off the PHYs of the embedded switch
 * of its Marvell FFxAV (88E7251).
 *
 * The board's LED module (ffxav_led.ko) drives the phone LED, and part of
 * the power LED, through the LED override register of a switch PHY:
 * register 25 of PHY n, 3 forces the PHY's LED0 on, 2 forces it off (the
 * other fields 0: the PHY's other LEDs keep their normal function). The
 * PHY registers are memory mapped, 32-bit, data in bits 15:0, at
 * 0x8000e000 + (n << 7) + (reg << 2), in the window of the switch that the
 * Ethernet driver owns; we map them without requesting the region, and
 * only ever write the override register (and, once at probe, the LED
 * select register 22 of PHY 0, as the board's module does).
 *
 * Each child node is one LED: reg = the PHY number; label, color,
 * function and default-state as for any LED. default-state "keep" (the
 * default) leaves the register as it is.
 *
 * Written for the v90d project from the board's LED module
 * (SPI-LED-WDT-NOTES.md); no vendor code.
 */
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define PHY_NUM		7
#define PHY_REG(n, r)	(((n) << 7) | ((r) << 2))
#define PHY_LED_SELECT	22
#define PHY_LED_FORCE	25
#define FORCE_ON	3
#define FORCE_OFF	2

struct ffxav_phy_led {
	struct led_classdev cdev;
	void __iomem *reg;
};

static void ffxav_phy_led_set(struct led_classdev *cdev,
			      enum led_brightness b)
{
	struct ffxav_phy_led *l = container_of(cdev, struct ffxav_phy_led, cdev);

	writel(b ? FORCE_ON : FORCE_OFF, l->reg);
}

static enum led_brightness ffxav_phy_led_get(struct led_classdev *cdev)
{
	struct ffxav_phy_led *l = container_of(cdev, struct ffxav_phy_led, cdev);

	return (readl(l->reg) & 3) == FORCE_ON ? LED_FULL : LED_OFF;
}

static int ffxav_phy_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;
	struct resource *res;
	void __iomem *base;
	u32 sel;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || resource_size(res) < PHY_REG(PHY_NUM, 0))
		return -EINVAL;
	/* shared with the Ethernet driver: not requested */
	base = devm_ioremap(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	if (!device_property_read_u32(dev, "marvell,led-select", &sel)) {
		u32 old = readl(base + PHY_REG(0, PHY_LED_SELECT));

		writel(sel, base + PHY_REG(0, PHY_LED_SELECT));
		dev_info(dev, "phy 0 register 22: %04x -> %04x\n", old,
			 readl(base + PHY_REG(0, PHY_LED_SELECT)));
	}

	device_for_each_child_node(dev, child) {
		struct led_init_data init = { .fwnode = child };
		struct ffxav_phy_led *l;
		u32 phy;

		if (fwnode_property_read_u32(child, "reg", &phy) ||
		    phy >= PHY_NUM) {
			fwnode_handle_put(child);
			return dev_err_probe(dev, -EINVAL, "bad reg in %pfw\n",
					     child);
		}
		l = devm_kzalloc(dev, sizeof(*l), GFP_KERNEL);
		if (!l) {
			fwnode_handle_put(child);
			return -ENOMEM;
		}
		l->reg = base + PHY_REG(phy, PHY_LED_FORCE);
		l->cdev.max_brightness = 1;
		l->cdev.brightness_set = ffxav_phy_led_set;
		l->cdev.brightness_get = ffxav_phy_led_get;
		fwnode_property_read_string(child, "linux,default-trigger",
					    &l->cdev.default_trigger);

		dev_info(dev, "phy %u register 25: %04x\n", phy, readl(l->reg));
		switch (led_init_default_state_get(child)) {
		case LEDS_DEFSTATE_ON:
			ffxav_phy_led_set(&l->cdev, LED_FULL);
			break;
		case LEDS_DEFSTATE_OFF:
			ffxav_phy_led_set(&l->cdev, LED_OFF);
			break;
		default:
			break;
		}
		l->cdev.brightness = ffxav_phy_led_get(&l->cdev);

		ret = devm_led_classdev_register_ext(dev, &l->cdev, &init);
		if (ret) {
			fwnode_handle_put(child);
			return ret;
		}
	}
	return 0;
}

static const struct of_device_id ffxav_phy_leds_of_match[] = {
	{ .compatible = "marvell,ffxav-phy-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, ffxav_phy_leds_of_match);

static struct platform_driver ffxav_phy_leds_driver = {
	.probe = ffxav_phy_leds_probe,
	.driver = {
		.name = "ffxav-phy-leds",
		.of_match_table = ffxav_phy_leds_of_match,
	},
};
module_platform_driver(ffxav_phy_leds_driver);

MODULE_DESCRIPTION("LEDs on the switch PHYs of the Marvell FFxAV");
MODULE_LICENSE("GPL");
