// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mdio.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/mfd/rtl8231.h>

static bool rtl8231_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	/*
	 * Registers with self-clearing bits, strapping pin values.
	 * Don't mark the data registers as volatile, since we need
	 * caching for the output values.
	 */
	case RTL8231_REG_FUNC0:
	case RTL8231_REG_FUNC1:
	case RTL8231_REG_PIN_HI_CFG:
	case RTL8231_REG_LED_END:
		return true;
	default:
		return false;
	}
}

static const struct mfd_cell rtl8231_cells[] = {
	{
		.name = "rtl8231-pinctrl",
	},
	{
		.name = "rtl8231-leds",
		.of_compatible = "realtek,rtl8231-leds",
	},
};

static int rtl8231_soft_reset(struct regmap *regmap)
{
	const unsigned int all_pins_mask = GENMASK(RTL8231_BITS_VAL - 1, 0);
	unsigned int cfg;
	int err;

	/* SOFT_RESET bit self-clears when done */
	regmap_write_bits(regmap, RTL8231_REG_PIN_HI_CFG,
			  RTL8231_PIN_HI_CFG_SOFT_RESET, RTL8231_PIN_HI_CFG_SOFT_RESET);

	err = regmap_read_poll_timeout(regmap, RTL8231_REG_PIN_HI_CFG, cfg,
				       !(cfg & RTL8231_PIN_HI_CFG_SOFT_RESET), 50, 1000);
	if (err)
		return err;

	regcache_drop_region(regmap, 0, RTL8231_REG_COUNT - 1);

	/*
	 * Chip reset results in a pin configuration that is a mix of LED and GPIO outputs.
	 * Select GPI functionality for all pins before enabling pin outputs.
	 */
	regmap_write(regmap, RTL8231_REG_PIN_MODE0, all_pins_mask);
	regmap_write(regmap, RTL8231_REG_GPIO_DIR0, all_pins_mask);
	regmap_write(regmap, RTL8231_REG_PIN_MODE1, all_pins_mask);
	regmap_write(regmap, RTL8231_REG_GPIO_DIR1, all_pins_mask);
	regmap_write(regmap, RTL8231_REG_PIN_HI_CFG,
		     RTL8231_PIN_HI_CFG_MODE_MASK | RTL8231_PIN_HI_CFG_DIR_MASK);

	return 0;
}

static int rtl8231_init(struct device *dev, struct regmap *regmap)
{
	struct regmap_field *led_start;
	unsigned int ready_code;
	unsigned int started;
	unsigned int status;
	int err;

	err = regmap_read(regmap, RTL8231_REG_FUNC1, &status);
	if (err) {
		dev_err(dev, "failed to read READY_CODE\n");
		return err;
	}

	ready_code = FIELD_GET(RTL8231_FUNC1_READY_CODE_MASK, status);
	if (ready_code != RTL8231_FUNC1_READY_CODE_VALUE) {
		dev_err(dev, "RTL8231 not present or ready 0x%x != 0x%x\n",
			ready_code, RTL8231_FUNC1_READY_CODE_VALUE);
		return -ENODEV;
	}

	led_start = dev_get_drvdata(dev);
	err = regmap_field_read(led_start, &started);
	if (err)
		return err;

	if (started)
		return 0;

	err = rtl8231_soft_reset(regmap);
	if (err)
		return err;

	/* LED_START enables power to output pins, and starts the LED engine */
	return regmap_field_write(led_start, 1);
}

static const struct regmap_config rtl8231_mdio_regmap_config = {
	.val_bits = RTL8231_BITS_VAL,
	.reg_bits = RTL8231_BITS_REG,
	.volatile_reg = rtl8231_volatile_reg,
	.max_register = RTL8231_REG_COUNT - 1,
	.use_single_read = true,
	.use_single_write = true,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_MAPLE,
};

static int rtl8231_mdio_probe(struct mdio_device *mdiodev)
{
	const struct reg_field field_led_start = REG_FIELD(RTL8231_REG_FUNC0, 1, 1);
	struct device *dev = &mdiodev->dev;
	struct regmap_field *led_start;
	struct regmap *regmap;
	int err;

	regmap = devm_regmap_init_mdio(mdiodev, &rtl8231_mdio_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(dev, "failed to init regmap\n");
		return PTR_ERR(regmap);
	}

	led_start = devm_regmap_field_alloc(dev, regmap, field_led_start);
	if (IS_ERR(led_start))
		return PTR_ERR(led_start);

	dev_set_drvdata(dev, led_start);

	mdiodev->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(mdiodev->reset_gpio))
		return PTR_ERR(mdiodev->reset_gpio);

	device_property_read_u32(dev, "reset-assert-delay", &mdiodev->reset_assert_delay);
	device_property_read_u32(dev, "reset-deassert-delay", &mdiodev->reset_deassert_delay);

	err = rtl8231_init(dev, regmap);
	if (err)
		return err;

	return devm_mfd_add_devices(dev, PLATFORM_DEVID_AUTO, rtl8231_cells,
				    ARRAY_SIZE(rtl8231_cells), NULL, 0, NULL);
}

static int __maybe_unused rtl8231_suspend(struct device *dev)
{
	struct regmap_field *led_start = dev_get_drvdata(dev);

	return regmap_field_write(led_start, 0);
}

static int __maybe_unused rtl8231_resume(struct device *dev)
{
	struct regmap_field *led_start = dev_get_drvdata(dev);

	return regmap_field_write(led_start, 1);
}

static SIMPLE_DEV_PM_OPS(rtl8231_pm_ops, rtl8231_suspend, rtl8231_resume);

static const struct of_device_id rtl8231_of_match[] = {
	{ .compatible = "realtek,rtl8231" },
	{}
};
MODULE_DEVICE_TABLE(of, rtl8231_of_match);

static struct mdio_driver rtl8231_mdio_driver = {
	.mdiodrv.driver = {
		.name = "rtl8231-expander",
		.of_match_table	= rtl8231_of_match,
		.pm = pm_ptr(&rtl8231_pm_ops),
	},
	.probe = rtl8231_mdio_probe,
};
mdio_module_driver(rtl8231_mdio_driver);

MODULE_AUTHOR("Sander Vanheule <sander@svanheule.net>");
MODULE_DESCRIPTION("Realtek RTL8231 GPIO and LED expander");
MODULE_LICENSE("GPL");
