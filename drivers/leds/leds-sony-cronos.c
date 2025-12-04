// SPDX-License-Identifier: GPL-2.0
/*
 * LED driver for Sony Cronos SMCs
 * Copyright (C) 2012 Dialog Semiconductor Ltd.
 * Copyright (C) 2023 Sony Interactive Entertainment
 * Copyright (C) 2025 Raptor Engineering, LLC
 */

#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/led-class-multicolor.h>
#include <linux/mfd/sony-cronos.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

/* Masks and Bit shifts */
#define CRONOS_LEDS_STATUS_FLASHING_MASK	0x40
#define CRONOS_LEDS_STATUS_FLASHING_SHIFT	6
#define CRONOS_LEDS_STATUS_COLOR_MASK		0x07
#define CRONOS_LEDS_STATUS_COLOR_SHIFT		0

#define CRONOS_LEDS_LINK_FLASHING_MASK		0x80
#define CRONOS_LEDS_LINK_FLASHING_SHIFT		7
#define CRONOS_LEDS_LINK_COLOR_MASK		0x38
#define CRONOS_LEDS_LINK_COLOR_SHIFT		3

#define CRONOS_LEDS_CCM1_POWER_COLOR_MASK	0x03
#define CRONOS_LEDS_CCM1_POWER_COLOR_SHIFT	0
#define CRONOS_LEDS_CCM2_POWER_COLOR_MASK	0x0C
#define CRONOS_LEDS_CCM2_POWER_COLOR_SHIFT	2
#define CRONOS_LEDS_CCM3_POWER_COLOR_MASK	0x30
#define CRONOS_LEDS_CCM3_POWER_COLOR_SHIFT	4
#define CRONOS_LEDS_CCM4_POWER_COLOR_MASK	0xC0
#define CRONOS_LEDS_CCM4_POWER_COLOR_SHIFT	6

/* LED Color mapping - Links and status LEDs */
#define LED_COLOR_OFF		0x00
#define LED_COLOR_BLUE		0x01
#define LED_COLOR_GREEN		0x02
#define LED_COLOR_RED		0x04

/* LED Color mapping - Power state LEDs */
#define LED_COLOR_POWER_OFF	0x00
#define LED_COLOR_POWER_RED	0x02
#define LED_COLOR_POWER_GREEN	0x01

/* Number of LEDs per type */
#define LED_COUNT_STATUS	6
#define LED_COUNT_LINK		5
#define LED_COUNT_POWER		4
#define LED_COUNT_ALL (LED_COUNT_STATUS + LED_COUNT_LINK + LED_COUNT_POWER)

enum sony_cronos_led_id {
	LED_ID_CCM1_STATUS = 0x00,
	LED_ID_CCM2_STATUS,
	LED_ID_CCM3_STATUS,
	LED_ID_CCM4_STATUS,
	LED_ID_SWITCH_STATUS,
	LED_ID_SMC_STATUS,

	LED_ID_CCM1_LINK,
	LED_ID_CCM2_LINK,
	LED_ID_CCM3_LINK,
	LED_ID_CCM4_LINK,
	LED_ID_SWITCH_LINK,

	LED_ID_CCM1_POWER,
	LED_ID_CCM2_POWER,
	LED_ID_CCM3_POWER,
	LED_ID_CCM4_POWER,

	LED_ID_COUNT,
};

enum sony_cronos_led_type {
	LED_TYPE_STATUS,
	LED_TYPE_LINK,
	LED_TYPE_POWER,
};

/**
 * struct sony_cronos_led - per-LED part of driver private data structure
 * @mc_cdev:		multi-color LED class device
 * @subled_info:	per-channel information
 * @led_register:	led register in the MFD regmap
 * @led_type:		sie_cronos_led_type
 * @led_id:		sie_cronos_led_id
 */
struct sony_cronos_led {
	struct led_classdev_mc mc_cdev;
	struct mc_subled subled_info[LED_COUNT_ALL];
	u8 led_register;
	enum sony_cronos_led_type led_type;
	enum sony_cronos_led_id led_id;
};

#define to_cronos_led(l) container_of(l, struct sony_cronos_led, mc_cdev)

/**
 * struct sony_cronos_leds - driver private data structure
 * @hw:				handle to hw device
 * @leds:			flexible array of per-LED data
 */
struct sony_cronos_leds {
	struct sony_cronos_smc *hw;
	struct sony_cronos_led leds[];
};

static int cronos_led_color_store(struct sony_cronos_smc *chip, struct sony_cronos_led *led)
{
	u8 byte;
	u8 color_mask;
	u8 color_shift;
	u8 color_key_red;
	u8 color_key_green;
	u8 color_key_blue;
	int ret;

	if (led->led_type == LED_TYPE_STATUS) {
		color_mask = CRONOS_LEDS_STATUS_COLOR_MASK;
		color_shift = CRONOS_LEDS_STATUS_COLOR_SHIFT;
	} else if (led->led_type == LED_TYPE_LINK) {
		color_mask = CRONOS_LEDS_LINK_COLOR_MASK;
		color_shift = CRONOS_LEDS_LINK_COLOR_SHIFT;
	} else if (led->led_id == LED_ID_CCM1_POWER) {
		color_mask = CRONOS_LEDS_CCM1_POWER_COLOR_MASK;
		color_shift = CRONOS_LEDS_CCM1_POWER_COLOR_SHIFT;
	} else if (led->led_id == LED_ID_CCM2_POWER) {
		color_mask = CRONOS_LEDS_CCM2_POWER_COLOR_MASK;
		color_shift = CRONOS_LEDS_CCM2_POWER_COLOR_SHIFT;
	} else if (led->led_id == LED_ID_CCM3_POWER) {
		color_mask = CRONOS_LEDS_CCM3_POWER_COLOR_MASK;
		color_shift = CRONOS_LEDS_CCM3_POWER_COLOR_SHIFT;
	} else if (led->led_id == LED_ID_CCM4_POWER) {
		color_mask = CRONOS_LEDS_CCM4_POWER_COLOR_MASK;
		color_shift = CRONOS_LEDS_CCM4_POWER_COLOR_SHIFT;
	} else
		return ret;

	switch (led->led_type) {
	case LED_TYPE_POWER:
		color_key_red = LED_COLOR_POWER_RED;
		color_key_green = LED_COLOR_POWER_GREEN;
		/* Blue channel does not exist for CCM power LEDs */
		color_key_blue = LED_COLOR_POWER_OFF;
		break;
	default:
		color_key_red = LED_COLOR_RED;
		color_key_green = LED_COLOR_GREEN;
		color_key_blue = LED_COLOR_BLUE;
	}

	/* Assemble SMC color command code */
	byte = LED_COLOR_POWER_OFF;
	if (led->subled_info[0].brightness > 128)
		byte |= color_key_red;
	if (led->subled_info[1].brightness > 128)
		byte |= color_key_green;
	if (led->subled_info[2].brightness > 128)
		byte |= color_key_blue;

	ret = regmap_update_bits(chip->regmap, led->led_register, color_mask, byte << color_shift);
	if (ret) {
		dev_err(chip->dev, "Failed to set color value 0x%02x to LED register 0x%02x", byte,
			led->led_register);
		return ret;
	}
	return 0;
}

static ssize_t cronos_led_set_brightness(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(cdev);
	struct sony_cronos_leds *leds = dev_get_drvdata(cdev->dev->parent);
	struct sony_cronos_led *led = to_cronos_led(mc_cdev);

	led_mc_calc_color_components(mc_cdev, brightness ?: cdev->max_brightness);

	return cronos_led_color_store(leds->hw, led);
}

static int sony_cronos_led_register(struct device *dev, struct sony_cronos_leds *leds,
				    struct sony_cronos_led *led, struct device_node *np)
{
	struct led_init_data init_data = {};
	struct led_classdev *cdev;
	int led_index;
	int ret, color;

	ret = of_property_read_u32(np, "reg", &led_index);
	if (ret || led_index >= LED_COUNT_ALL) {
		dev_err(dev, "'reg' property is out of range (0-%i)\n", LED_COUNT_ALL - 1);
		return -EINVAL;
	}

	switch (led_index) {
	case 0:
		led->led_register = CRONOS_LEDS_CCM1_STATUS_REG;
		led->led_type = LED_TYPE_STATUS;
		led->led_id = LED_ID_CCM1_STATUS;
		break;
	case 1:
		led->led_register = CRONOS_LEDS_CCM2_STATUS_REG;
		led->led_type = LED_TYPE_STATUS;
		led->led_id = LED_ID_CCM2_STATUS;
		break;
	case 2:
		led->led_register = CRONOS_LEDS_CCM3_STATUS_REG;
		led->led_type = LED_TYPE_STATUS;
		led->led_id = LED_ID_CCM3_STATUS;
		break;
	case 3:
		led->led_register = CRONOS_LEDS_CCM4_STATUS_REG;
		led->led_type = LED_TYPE_STATUS;
		led->led_id = LED_ID_CCM4_STATUS;
		break;
	case 4:
		led->led_register = CRONOS_LEDS_SWITCH_STATUS_REG;
		led->led_type = LED_TYPE_STATUS;
		led->led_id = LED_ID_SWITCH_STATUS;
		break;
	case 5:
		led->led_register = CRONOS_LEDS_SMC_STATUS_REG;
		led->led_type = LED_TYPE_STATUS;
		led->led_id = LED_ID_SMC_STATUS;
		break;
	case 6:
		led->led_register = CRONOS_LEDS_CCM1_STATUS_REG;
		led->led_type = LED_TYPE_LINK;
		led->led_id = LED_ID_CCM1_LINK;
		break;
	case 7:
		led->led_register = CRONOS_LEDS_CCM2_STATUS_REG;
		led->led_type = LED_TYPE_LINK;
		led->led_id = LED_ID_CCM1_LINK;
		break;
	case 8:
		led->led_register = CRONOS_LEDS_CCM3_STATUS_REG;
		led->led_type = LED_TYPE_LINK;
		led->led_id = LED_ID_CCM2_LINK;
		break;
	case 9:
		led->led_register = CRONOS_LEDS_CCM4_STATUS_REG;
		led->led_type = LED_TYPE_LINK;
		led->led_id = LED_ID_CCM3_LINK;
		break;
	case 10:
		led->led_register = CRONOS_LEDS_SWITCH_STATUS_REG;
		led->led_type = LED_TYPE_LINK;
		led->led_id = LED_ID_CCM4_LINK;
		break;
	case 11:
		led->led_register = CRONOS_LEDS_CCM_POWER_REG;
		led->led_type = LED_TYPE_POWER;
		led->led_id = LED_ID_CCM1_POWER;
		break;
	case 12:
		led->led_register = CRONOS_LEDS_CCM_POWER_REG;
		led->led_type = LED_TYPE_POWER;
		led->led_id = LED_ID_CCM2_POWER;
		break;
	case 13:
		led->led_register = CRONOS_LEDS_CCM_POWER_REG;
		led->led_type = LED_TYPE_POWER;
		led->led_id = LED_ID_CCM3_POWER;
		break;
	case 14:
		led->led_register = CRONOS_LEDS_CCM_POWER_REG;
		led->led_type = LED_TYPE_POWER;
		led->led_id = LED_ID_CCM4_POWER;
		break;
	default:
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "color", &color);
	if (ret || color != LED_COLOR_ID_RGB) {
		dev_warn(dev,
			 "Node %pOF: must contain 'color' property with value LED_COLOR_ID_RGB\n",
			 np);
		return -EINVAL;
	}

	led->subled_info[0].color_index = LED_COLOR_ID_RED;
	led->subled_info[1].color_index = LED_COLOR_ID_GREEN;
	led->subled_info[2].color_index = LED_COLOR_ID_BLUE;

	/* Initial color is white */
	for (int i = 0; i < LED_COUNT_ALL; i++) {
		led->subled_info[i].intensity = 255;
		led->subled_info[i].brightness = 255;
		led->subled_info[i].channel = i;
	}

	led->mc_cdev.subled_info = led->subled_info;
	led->mc_cdev.num_colors = LED_COUNT_ALL;

	init_data.fwnode = &np->fwnode;

	cdev = &led->mc_cdev.led_cdev;
	cdev->max_brightness = 255;
	cdev->brightness_set_blocking = cronos_led_set_brightness;

	/* Set initial color */
	ret = cronos_led_color_store(leds->hw, led);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Cannot set LED %pOF initial color\n", np);

	ret = devm_led_classdev_multicolor_register_ext(dev, &led->mc_cdev, &init_data);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register LED %pOF\n", np);

	/* Set global brightness for all LEDs */
	ret = regmap_write(leds->hw->regmap, CRONOS_SMC_BRIGHTNESS_RED_REG, 0x00);
	ret = regmap_write(leds->hw->regmap, CRONOS_SMC_BRIGHTNESS_GREEN_REG, 0x00);
	ret = regmap_write(leds->hw->regmap, CRONOS_SMC_BRIGHTNESS_BLUE_REG, 0x00);

	return 0;
}

static int sony_cronos_leds_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev_of_node(dev);
	struct sony_cronos_smc *chip;
	struct sony_cronos_leds *leds;
	struct sony_cronos_led *led;
	int ret, count;

	chip = dev_get_drvdata(dev->parent);
	if (!chip)
		return -EINVAL;

	count = of_get_available_child_count(np);
	if (count == 0)
		return dev_err_probe(dev, -ENODEV, "LEDs are not defined in device tree!\n");
	if (count > LED_COUNT_ALL)
		return dev_err_probe(dev, -EINVAL, "Too many LEDs defined in device tree!\n");

	leds = devm_kzalloc(dev, struct_size(leds, leds, count), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	leds->hw = chip;

	led = &leds->leds[0];
	for_each_available_child_of_node_scoped(np, child) {
		ret = sony_cronos_led_register(dev, leds, led, child);
		if (ret)
			return ret;

		led++;
	}

	return 0;
}

static const struct of_device_id sony_cronos_led_of_id_table[] = {
	{ .compatible = "sie,cronos-led", },
	{},
};
MODULE_DEVICE_TABLE(of, sony_cronos_led_of_id_table);

static struct platform_driver sony_cronos_led_driver = {
	.driver = {
		.name = "sie-cronos-led",
		.of_match_table = sony_cronos_led_of_id_table,
	},
	.probe = sony_cronos_leds_probe,
};
module_platform_driver(sony_cronos_led_driver);

MODULE_DESCRIPTION("LED driver for SIE Cronos SMCs");
MODULE_AUTHOR("Timothy Pearson <tpearson@raptorengineering.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sony-cronos-leds");
