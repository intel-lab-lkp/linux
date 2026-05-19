// SPDX-License-Identifier: GPL-2.0
/*
 * LTC3220 18-Channel LED Driver
 *
 * Copyright 2026 Analog Devices Inc.
 *
 * Author: Edelweise Escala <edelweise.escala@analog.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/types.h>

/* LTC3220 Registers */
#define LTC3220_COMMAND_REG				0x00
#define   LTC3220_QUICK_WRITE_MASK			BIT(0)
#define   LTC3220_SHUTDOWN_MASK				BIT(3)

#define LTC3220_ULED_REG(x)				(0x01 + (x))
#define   LTC3220_LED_CURRENT_MASK			GENMASK(5, 0)
#define   LTC3220_LED_MODE_MASK				GENMASK(7, 6)

#define LTC3220_GRAD_BLINK_REG				0x13
#define   LTC3220_GRADATION_MASK			GENMASK(2, 0)
#define   LTC3220_GRADATION_DIRECTION_MASK		BIT(0)
#define   LTC3220_GRADATION_PERIOD_MASK			GENMASK(2, 1)
#define   LTC3220_BLINK_MASK				GENMASK(4, 3)

#define LTC3220_NUM_LEDS				18

#define LTC3220_GRADATION_START_VALUE			128
#define LTC3220_GRADATION_RAMP_TIME_240MS		240
#define LTC3220_GRADATION_RAMP_TIME_480MS		480

#define LTC3220_BLINK_ON_156MS				156
#define LTC3220_BLINK_ON_625MS				625
#define LTC3220_BLINK_PERIOD_1250MS			1250
#define LTC3220_BLINK_PERIOD_2500MS			2500

#define LTC3220_BLINK_SHORT_ON_TIME			BIT(0)
#define LTC3220_BLINK_LONG_PERIOD			BIT(1)

enum ltc3220_blink_mode {
	LTC3220_BLINK_MODE_625MS_1250MS,
	LTC3220_BLINK_MODE_156MS_1250MS,
	LTC3220_BLINK_MODE_625MS_2500MS,
	LTC3220_BLINK_MODE_156MS_2500MS
};

enum ltc3220_gradation_mode {
	LTC3220_GRADATION_MODE_DISABLED,
	LTC3220_GRADATION_MODE_240MS_RAMP_TIME,
	LTC3220_GRADATION_MODE_480MS_RAMP_TIME,
	LTC3220_GRADATION_MODE_960MS_RAMP_TIME
};

static bool ltc3220_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg == LTC3220_GRAD_BLINK_REG;
}

static const struct regmap_config ltc3220_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LTC3220_GRAD_BLINK_REG,
	.cache_type = REGCACHE_FLAT_S,
	.volatile_reg = ltc3220_volatile_reg,
};

struct ltc3220_uled_cfg {
	struct led_classdev led_cdev;
	u8 reg_value;
	u8 led_index;
};

struct ltc3220 {
	struct ltc3220_uled_cfg uled_cfg[LTC3220_NUM_LEDS];
	struct regmap *regmap;
	bool is_aggregated;
};

/*
 * Set LED brightness and mode.
 * The brightness value determines both the LED current and operating mode:
 * 0-63:    Normal mode - LED current from 0-63 (off to full brightness)
 * 64-127:  Blink mode - LED blinks with current level (brightness - 64)
 * 128-191: Gradation mode - LED gradually changes brightness (brightness - 128)
 * 192-255: GPO mode - LED operates as general purpose output (brightness - 192)
 */
static int ltc3220_set_led_data(struct led_classdev *led_cdev,
				enum led_brightness brightness)
{
	struct ltc3220_uled_cfg *uled_cfg = container_of(led_cdev, struct ltc3220_uled_cfg,
							 led_cdev);
	struct ltc3220 *ltc3220 = container_of(uled_cfg, struct ltc3220,
					       uled_cfg[uled_cfg->led_index]);
	int ret;

	ret = regmap_write(ltc3220->regmap, LTC3220_ULED_REG(uled_cfg->led_index),
			   brightness);
	if (ret)
		return ret;

	uled_cfg->reg_value = brightness;

	/*
	 * When aggregated LED mode is enabled, writing to LED 1 updates all
	 * LEDs simultaneously via quick-write mode. Update cached values for
	 * all LEDs to reflect the synchronized state.
	 * See Documentation/devicetree/bindings/leds/adi,ltc3220.yaml for how
	 * to configure aggregated LED mode.
	 */
	if (ltc3220->is_aggregated && uled_cfg->led_index == 0) {
		for (int i = 0; i < LTC3220_NUM_LEDS; i++)
			ltc3220->uled_cfg[i].reg_value = brightness;
	}

	return 0;
}

static enum led_brightness ltc3220_get_led_data(struct led_classdev *led_cdev)
{
	struct ltc3220_uled_cfg *uled_cfg = container_of(led_cdev, struct ltc3220_uled_cfg,
							 led_cdev);

	return uled_cfg->reg_value;
}

/*
 * LTC3220 pattern support for hardware-assisted breathing/gradation.
 * The hardware supports 3 gradation ramp time 240ms, 480ms, 960ms)
 * and can ramp up or down.
 *
 * Pattern array interpretation:
 *   pattern[0].brightness = start brightness (0-63)
 *   pattern[0].delta_t = ramp time in milliseconds
 *   pattern[1].brightness = end brightness (0-63)
 *   pattern[1].delta_t = (optional, can be 0 or same as pattern[0].delta_t)
 */
static int ltc3220_pattern_set(struct led_classdev *led_cdev,
			       struct led_pattern *pattern,
			       u32 len, int repeat)
{
	struct ltc3220_uled_cfg *uled_cfg = container_of(led_cdev, struct ltc3220_uled_cfg,
							 led_cdev);
	struct ltc3220 *ltc3220 = container_of(uled_cfg, struct ltc3220,
					       uled_cfg[uled_cfg->led_index]);
	u8 gradation_period;
	u8 start_brightness;
	u8 end_brightness;
	u8 gradation_val;
	bool is_increasing;
	int ret;

	if (len != 2)
		return -EINVAL;

	start_brightness = pattern[0].brightness & LTC3220_LED_CURRENT_MASK;
	end_brightness = pattern[1].brightness & LTC3220_LED_CURRENT_MASK;

	is_increasing = end_brightness > start_brightness;

	if (pattern[0].delta_t == 0)
		gradation_period = LTC3220_GRADATION_MODE_DISABLED;
	else if (pattern[0].delta_t <= LTC3220_GRADATION_RAMP_TIME_240MS)
		gradation_period = LTC3220_GRADATION_MODE_240MS_RAMP_TIME;
	else if (pattern[0].delta_t <= LTC3220_GRADATION_RAMP_TIME_480MS)
		gradation_period = LTC3220_GRADATION_MODE_480MS_RAMP_TIME;
	else
		gradation_period = LTC3220_GRADATION_MODE_960MS_RAMP_TIME;

	gradation_val = FIELD_PREP(LTC3220_GRADATION_PERIOD_MASK, gradation_period);
	gradation_val |= FIELD_PREP(LTC3220_GRADATION_DIRECTION_MASK, is_increasing);

	ret = regmap_update_bits(ltc3220->regmap, LTC3220_GRAD_BLINK_REG,
				 LTC3220_GRADATION_MASK, gradation_val);
	if (ret)
		return ret;

	ret = ltc3220_set_led_data(led_cdev, start_brightness);
	if (ret)
		return ret;

	return ltc3220_set_led_data(led_cdev, LTC3220_GRADATION_START_VALUE + end_brightness);
}

static int ltc3220_pattern_clear(struct led_classdev *led_cdev)
{
	struct ltc3220_uled_cfg *uled_cfg = container_of(led_cdev, struct ltc3220_uled_cfg,
							 led_cdev);
	struct ltc3220 *ltc3220 = container_of(uled_cfg, struct ltc3220,
					       uled_cfg[uled_cfg->led_index]);

	return regmap_update_bits(ltc3220->regmap, LTC3220_GRAD_BLINK_REG,
				  LTC3220_GRADATION_MASK, 0);
}

/*
 * LTC3220 has a global blink configuration that affects all LEDs.
 * This implementation allows per-LED blink requests, but the blink timing
 * will be shared across all LEDs. The delay values are mapped to the
 * hardware's discrete blink rates.
 */
static int ltc3220_blink_set(struct led_classdev *led_cdev,
			     unsigned long *delay_on,
			     unsigned long *delay_off)
{
	struct ltc3220_uled_cfg *uled_cfg = container_of(led_cdev, struct ltc3220_uled_cfg,
							 led_cdev);
	struct ltc3220 *ltc3220 = container_of(uled_cfg, struct ltc3220,
					       uled_cfg[uled_cfg->led_index]);
	u8 blink_mode = 0;

	if (*delay_on <= LTC3220_BLINK_ON_156MS)
		blink_mode = LTC3220_BLINK_SHORT_ON_TIME;

	if (*delay_on + *delay_off > LTC3220_BLINK_PERIOD_1250MS)
		blink_mode |= LTC3220_BLINK_LONG_PERIOD;

	switch (blink_mode) {
	case LTC3220_BLINK_MODE_625MS_1250MS:
		*delay_on = LTC3220_BLINK_ON_625MS;
		*delay_off = LTC3220_BLINK_PERIOD_1250MS - LTC3220_BLINK_ON_625MS;
		break;
	case LTC3220_BLINK_MODE_156MS_1250MS:
		*delay_on = LTC3220_BLINK_ON_156MS;
		*delay_off = LTC3220_BLINK_PERIOD_1250MS - LTC3220_BLINK_ON_156MS;
		break;
	case LTC3220_BLINK_MODE_625MS_2500MS:
		*delay_on = LTC3220_BLINK_ON_625MS;
		*delay_off = LTC3220_BLINK_PERIOD_2500MS - LTC3220_BLINK_ON_625MS;
		break;
	case LTC3220_BLINK_MODE_156MS_2500MS:
		*delay_on = LTC3220_BLINK_ON_156MS;
		*delay_off = LTC3220_BLINK_PERIOD_2500MS - LTC3220_BLINK_ON_156MS;
		break;
	}

	return regmap_update_bits(ltc3220->regmap, LTC3220_GRAD_BLINK_REG,
				  LTC3220_BLINK_MASK, blink_mode);
}

static void ltc3220_reset_gpio_action(void *data)
{
	struct gpio_desc *reset_gpio = data;

	gpiod_set_value_cansleep(reset_gpio, 1);
}

static int ltc3220_reset(struct ltc3220 *ltc3220, struct i2c_client *client)
{
	struct gpio_desc *reset_gpio;
	int ret;

	reset_gpio = devm_gpiod_get_optional(&client->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(reset_gpio), "Failed on reset GPIO\n");

	if (reset_gpio) {
		gpiod_set_value_cansleep(reset_gpio, 0);

		return devm_add_action_or_reset(&client->dev, ltc3220_reset_gpio_action,
						reset_gpio);
	}

	ret = regmap_write(ltc3220->regmap, LTC3220_COMMAND_REG, 0);
	if (ret)
		return ret;

	for (int i = 0; i < LTC3220_NUM_LEDS; i++) {
		ret = regmap_write(ltc3220->regmap, LTC3220_ULED_REG(i), 0);
		if (ret)
			return ret;
	}

	return regmap_write(ltc3220->regmap, LTC3220_GRAD_BLINK_REG, 0);
}

static int ltc3220_suspend(struct device *dev)
{
	struct ltc3220 *ltc3220 = i2c_get_clientdata(to_i2c_client(dev));

	return regmap_update_bits(ltc3220->regmap, LTC3220_COMMAND_REG,
				  LTC3220_SHUTDOWN_MASK, LTC3220_SHUTDOWN_MASK);
}

static int ltc3220_resume(struct device *dev)
{
	struct ltc3220 *ltc3220 = i2c_get_clientdata(to_i2c_client(dev));

	return regmap_update_bits(ltc3220->regmap, LTC3220_COMMAND_REG,
				  LTC3220_SHUTDOWN_MASK, 0);
}

static DEFINE_SIMPLE_DEV_PM_OPS(ltc3220_pm_ops, ltc3220_suspend, ltc3220_resume);

static int ltc3220_probe(struct i2c_client *client)
{
	struct ltc3220 *ltc3220;
	bool aggregated_led_found = false;
	int num_leds = 0;
	u8 led_index = 0;
	int ret;

	ltc3220 = devm_kzalloc(&client->dev, sizeof(*ltc3220), GFP_KERNEL);
	if (!ltc3220)
		return -ENOMEM;

	ltc3220->regmap = devm_regmap_init_i2c(client, &ltc3220_regmap_config);
	if (IS_ERR(ltc3220->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(ltc3220->regmap),
				     "Failed to initialize regmap\n");

	i2c_set_clientdata(client, ltc3220);

	ret = ltc3220_reset(ltc3220, client);
	if (ret)
		return dev_err_probe(&client->dev, ret, "Failed to reset device\n");

	device_for_each_child_node_scoped(&client->dev, child) {
		struct led_init_data init_data = {};
		struct ltc3220_uled_cfg *led;
		u32 source;

		ret = fwnode_property_read_u32(child, "reg", &source);
		if (ret)
			return dev_err_probe(&client->dev, ret, "Couldn't read LED address\n");

		if (!source || source > LTC3220_NUM_LEDS)
			return dev_err_probe(&client->dev, -EINVAL, "LED address out of range\n");

		init_data.fwnode = child;
		init_data.devicename = "ltc3220";

		if (fwnode_property_present(child, "led-sources")) {
			if (source != 1)
				return dev_err_probe(&client->dev, -EINVAL,
						     "Aggregated LED out of range\n");

			if (aggregated_led_found)
				return dev_err_probe(&client->dev, -EINVAL,
						     "One Aggregated LED only\n");

			aggregated_led_found = true;
			ltc3220->is_aggregated = true;

			ret = regmap_update_bits(ltc3220->regmap,
						 LTC3220_COMMAND_REG,
						 LTC3220_QUICK_WRITE_MASK,
						 LTC3220_QUICK_WRITE_MASK);
			if (ret)
				return dev_err_probe(&client->dev, ret,
						     "Failed to set quick write mode\n");
		}

		num_leds++;

		/* LED node reg/index/address goes from 1 to 18 */
		led_index = source - 1;
		led = &ltc3220->uled_cfg[led_index];
		led->led_index = led_index;
		led->reg_value = 0;
		led->led_cdev.brightness_set_blocking = ltc3220_set_led_data;
		led->led_cdev.brightness_get = ltc3220_get_led_data;
		led->led_cdev.max_brightness = 255;
		led->led_cdev.blink_set = ltc3220_blink_set;
		led->led_cdev.pattern_set = ltc3220_pattern_set;
		led->led_cdev.pattern_clear = ltc3220_pattern_clear;

		ret = devm_led_classdev_register_ext(&client->dev, &led->led_cdev, &init_data);
		if (ret)
			return dev_err_probe(&client->dev, ret, "Failed to register LED class\n");
	}

	/*
	 * Aggregated LED mode uses hardware quick-write to control all 18 LEDs
	 * simultaneously. This is mutually exclusive with individual LED control.
	 * See Documentation/devicetree/bindings/leds/adi,ltc3220.yaml for details
	 * on how to configure aggregated LED mode.
	 */
	if (aggregated_led_found && num_leds > 1)
		return dev_err_probe(&client->dev, -EINVAL,
				     "Aggregated LED must be the only LED node\n");

	return 0;
}

static const struct of_device_id ltc3220_of_match[] = {
	{ .compatible = "adi,ltc3220" },
	{ }
};
MODULE_DEVICE_TABLE(of, ltc3220_of_match);

static struct i2c_driver ltc3220_led_driver = {
	.driver = {
		.name = "ltc3220",
		.of_match_table = ltc3220_of_match,
		.pm = pm_sleep_ptr(&ltc3220_pm_ops),
	},
	.probe = ltc3220_probe,
};
module_i2c_driver(ltc3220_led_driver);

MODULE_AUTHOR("Edelweise Escala <edelweise.escala@analog.com>");
MODULE_DESCRIPTION("LED driver for LTC3220 controllers");
MODULE_LICENSE("GPL");
