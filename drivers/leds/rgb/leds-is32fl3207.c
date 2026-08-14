// SPDX-License-Identifier: GPL-2.0-only
/*
 * ISSI IS32FL3207 LED controller driver
 *
 * Copyright 2026 Ahmad Byagowi
 */

#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/led-class-multicolor.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>

#include <dt-bindings/leds/common.h>

#define IS32FL3207_NUM_CHANNELS		18
#define IS32FL3207_MAX_BRIGHTNESS	255

#define IS32FL3207_REG_CONTROL		0x00
#define IS32FL3207_REG_PWM_LOW(channel)	(0x01 + 2 * (channel))
#define IS32FL3207_REG_PWM_UPDATE	0x49
#define IS32FL3207_REG_SCALING(channel)	(0x4a + (channel))
#define IS32FL3207_REG_GLOBAL_CURRENT	0x6e
#define IS32FL3207_REG_RESET		0x7f

#define IS32FL3207_CONTROL_ENABLE	BIT(0)
#define IS32FL3207_GLOBAL_CURRENT_MAX	0xff

/* IOUT(MAX) in microamperes = 76,500,000 / RISET in ohms. */
#define IS32FL3207_CURRENT_NUMERATOR	76500000ULL

struct is32fl3207;

struct is32fl3207_led {
	struct is32fl3207 *chip;
	struct led_classdev *led_cdev;
	union {
		struct led_classdev cdev;
		struct led_classdev_mc mcdev;
	};
	unsigned int channel;
};

struct is32fl3207 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *enable_gpio;
	struct mutex lock; /* Serializes controller register updates. */
	unsigned long channels[BITS_TO_LONGS(IS32FL3207_NUM_CHANNELS)];
	u32 output_max_microamp;
	unsigned int num_leds;
	bool shutting_down;
	struct is32fl3207_led leds[] __counted_by(num_leds);
};

static int is32fl3207_parse_led_properties(struct is32fl3207 *chip,
					   struct fwnode_handle *fwnode,
					   unsigned int *max_brightness,
					   unsigned int *brightness)
{
	enum led_default_state default_state;
	u32 value;
	int ret;

	*max_brightness = IS32FL3207_MAX_BRIGHTNESS;
	if (fwnode_property_present(fwnode, "max-brightness")) {
		ret = fwnode_property_read_u32(fwnode, "max-brightness",
					       &value);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read maximum brightness for %pfw\n",
					     fwnode);
		if (!value || value > IS32FL3207_MAX_BRIGHTNESS)
			return dev_err_probe(chip->dev, -EINVAL,
					     "invalid maximum brightness %u for %pfw\n",
					     value, fwnode);

		*max_brightness = value;
	}

	value = *max_brightness;
	if (fwnode_property_present(fwnode, "default-brightness")) {
		ret = fwnode_property_read_u32(fwnode, "default-brightness",
					       &value);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read default brightness for %pfw\n",
					     fwnode);
		if (value > *max_brightness)
			return dev_err_probe(chip->dev, -EINVAL,
					     "invalid default brightness %u for %pfw\n",
					     value, fwnode);
	}

	default_state = led_init_default_state_get(fwnode);
	if (default_state == LEDS_DEFSTATE_KEEP)
		return dev_err_probe(chip->dev, -EINVAL,
				     "default state keep is not supported for %pfw\n",
				     fwnode);

	*brightness = default_state == LEDS_DEFSTATE_ON ? value : LED_OFF;
	return 0;
}

static int is32fl3207_validate_component(struct is32fl3207 *chip,
					 struct fwnode_handle *fwnode)
{
	static const char * const unsupported[] = {
		"default-brightness",
		"default-state",
		"max-brightness",
		"retain-state-shutdown",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(unsupported); i++)
		if (fwnode_property_present(fwnode, unsupported[i]))
			return dev_err_probe(chip->dev, -EINVAL,
					     "%s is not supported for component %pfw\n",
					     unsupported[i], fwnode);

	return 0;
}

static int is32fl3207_write_channels_locked(struct is32fl3207 *chip,
					    const struct mc_subled *subleds,
					    unsigned int num_channels)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_channels; i++) {
		ret = regmap_write(chip->regmap,
				   IS32FL3207_REG_PWM_LOW(subleds[i].channel),
				   subleds[i].brightness);
		if (ret)
			return ret;
	}

	return regmap_write(chip->regmap, IS32FL3207_REG_PWM_UPDATE, 0);
}

static int is32fl3207_brightness_set(struct led_classdev *cdev,
				     enum led_brightness brightness)
{
	struct is32fl3207_led *led = container_of(cdev, struct is32fl3207_led,
						     cdev);
	struct mc_subled subled = {
		.brightness = brightness,
		.channel = led->channel,
	};

	guard(mutex)(&led->chip->lock);
	if (led->chip->shutting_down)
		return 0;

	return is32fl3207_write_channels_locked(led->chip, &subled, 1);
}

static int is32fl3207_mc_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct led_classdev_mc *mcdev = lcdev_to_mccdev(cdev);
	struct is32fl3207_led *led = container_of(mcdev, struct is32fl3207_led,
						     mcdev);

	guard(mutex)(&led->chip->lock);
	if (led->chip->shutting_down)
		return 0;

	led_mc_calc_color_components(mcdev, brightness);

	return is32fl3207_write_channels_locked(led->chip, mcdev->subled_info,
						   mcdev->num_colors);
}

static int is32fl3207_turn_off_locked(struct is32fl3207 *chip,
				      struct is32fl3207_led *led)
{
	struct led_classdev *cdev = led->led_cdev;
	struct mc_subled subled = {
		.brightness = LED_OFF,
		.channel = led->channel,
	};

	if (cdev->flags & LED_MULTI_COLOR) {
		struct led_classdev_mc *mcdev = lcdev_to_mccdev(cdev);

		led_mc_calc_color_components(mcdev, LED_OFF);
		return is32fl3207_write_channels_locked(chip,
							   mcdev->subled_info,
							   mcdev->num_colors);
	}

	return is32fl3207_write_channels_locked(chip, &subled, 1);
}

static int is32fl3207_configure_channel(struct is32fl3207 *chip,
					struct fwnode_handle *fwnode,
					unsigned int *channel)
{
	u64 scaling;
	u32 max_microamp;
	u32 reg;
	int ret;

	ret = fwnode_property_read_u32(fwnode, "reg", &reg);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read channel for %pfw\n",
				     fwnode);

	if (reg >= IS32FL3207_NUM_CHANNELS)
		return dev_err_probe(chip->dev, -EINVAL,
				     "channel %u is out of range\n", reg);

	if (test_bit(reg, chip->channels))
		return dev_err_probe(chip->dev, -EINVAL,
				     "channel %u is used more than once\n",
				     reg);

	ret = fwnode_property_read_u32(fwnode, "led-max-microamp",
				       &max_microamp);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read current limit for channel %u\n",
				     reg);

	if (!max_microamp || max_microamp > chip->output_max_microamp)
		return dev_err_probe(chip->dev, -EINVAL,
				     "invalid current limit %u uA for channel %u\n",
				     max_microamp, reg);

	/* GCC is fixed at 0xff, so use each output's scaling register. */
	scaling = div_u64((u64)max_microamp * 256 * 256,
			  (u64)chip->output_max_microamp *
			  IS32FL3207_GLOBAL_CURRENT_MAX);
	if (!scaling)
		return dev_err_probe(chip->dev, -EINVAL,
				     "current limit %u uA is below channel %u resolution\n",
				     max_microamp, reg);

	scaling = min_t(u64, scaling, 0xff);

	guard(mutex)(&chip->lock);
	ret = regmap_write(chip->regmap, IS32FL3207_REG_SCALING(reg),
			   (unsigned int)scaling);
	if (ret)
		return ret;

	set_bit(reg, chip->channels);
	*channel = reg;

	return 0;
}

static int is32fl3207_register_single(struct is32fl3207 *chip,
				      struct fwnode_handle *fwnode,
				      struct is32fl3207_led *led)
{
	struct led_init_data init_data = {
		.devicename = dev_name(chip->dev),
		.devname_mandatory = true,
		.fwnode = fwnode,
	};
	unsigned int max_brightness;
	unsigned int brightness;
	u32 color;
	int ret;

	if (!fwnode_property_present(fwnode, "function") &&
	    !fwnode_property_present(fwnode, "color"))
		return dev_err_probe(chip->dev, -EINVAL,
				     "single LED %pfw requires function or color\n",
				     fwnode);

	ret = is32fl3207_parse_led_properties(chip, fwnode,
					      &max_brightness, &brightness);
	if (ret)
		return ret;

	if (fwnode_property_present(fwnode, "color")) {
		ret = fwnode_property_read_u32(fwnode, "color", &color);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read color for %pfw\n",
					     fwnode);
		if (color >= LED_COLOR_ID_MAX || color == LED_COLOR_ID_MULTI ||
		    color == LED_COLOR_ID_RGB)
			return dev_err_probe(chip->dev, -EINVAL,
					     "invalid single LED color %u\n",
					     color);
	}

	ret = is32fl3207_configure_channel(chip, fwnode, &led->channel);
	if (ret)
		return ret;
	led->chip = chip;
	led->led_cdev = &led->cdev;
	led->cdev.brightness = brightness;
	led->cdev.max_brightness = max_brightness;
	led->cdev.brightness_set_blocking = is32fl3207_brightness_set;

	ret = is32fl3207_brightness_set(&led->cdev, brightness);
	if (ret)
		return ret;

	return devm_led_classdev_register_ext(chip->dev, &led->cdev,
					      &init_data);
}

static int is32fl3207_register_multicolor(struct is32fl3207 *chip,
					  struct fwnode_handle *fwnode,
					  struct is32fl3207_led *led)
{
	struct led_init_data init_data = {
		.devicename = dev_name(chip->dev),
		.devname_mandatory = true,
		.fwnode = fwnode,
	};
	struct mc_subled *subleds;
	DECLARE_BITMAP(color_map, LED_COLOR_ID_MAX);
	unsigned int max_brightness;
	unsigned int brightness;
	unsigned int count;
	unsigned int i = 0;
	bool has_group_reg;
	u32 group_color;
	u32 group_reg;
	unsigned int first_channel = IS32FL3207_NUM_CHANNELS;
	int ret;

	ret = is32fl3207_parse_led_properties(chip, fwnode,
					      &max_brightness, &brightness);
	if (ret)
		return ret;

	ret = fwnode_property_read_u32(fwnode, "color", &group_color);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read color for %pfw\n", fwnode);

	if (group_color != LED_COLOR_ID_RGB &&
	    group_color != LED_COLOR_ID_MULTI)
		return dev_err_probe(chip->dev, -EINVAL,
				     "invalid multicolor LED color %u\n",
				     group_color);

	has_group_reg = fwnode_property_present(fwnode, "reg");
	if (has_group_reg) {
		ret = fwnode_property_read_u32(fwnode, "reg", &group_reg);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read group index for %pfw\n",
					     fwnode);
	}

	count = fwnode_get_child_node_count(fwnode);
	if (!count || count > LED_COLOR_ID_MAX)
		return dev_err_probe(chip->dev, -EINVAL,
				     "invalid component count %u for %pfw\n",
				     count, fwnode);

	subleds = devm_kcalloc(chip->dev, count, sizeof(*subleds), GFP_KERNEL);
	if (!subleds)
		return -ENOMEM;
	bitmap_zero(color_map, LED_COLOR_ID_MAX);

	fwnode_for_each_child_node_scoped(fwnode, child) {
		u32 color;

		ret = is32fl3207_validate_component(chip, child);
		if (ret)
			return ret;

		ret = fwnode_property_read_u32(child, "color", &color);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read color for %pfw\n",
					     child);

		if (color >= LED_COLOR_ID_MAX || color == LED_COLOR_ID_MULTI ||
		    color == LED_COLOR_ID_RGB)
			return dev_err_probe(chip->dev, -EINVAL,
					     "invalid component color %u\n",
					     color);
		if (test_and_set_bit(color, color_map))
			return dev_err_probe(chip->dev, -EINVAL,
					     "component color %u is used more than once\n",
					     color);

		ret = is32fl3207_configure_channel(chip, child,
						   &subleds[i].channel);
		if (ret)
			return ret;

		subleds[i].color_index = color;
		subleds[i].intensity = max_brightness;
		subleds[i].max_intensity = 0;
		first_channel = min(first_channel, subleds[i].channel);
		i++;
	}

	if (has_group_reg && group_reg != first_channel)
		return dev_err_probe(chip->dev, -EINVAL,
				     "group index %u does not match first channel %u\n",
				     group_reg, first_channel);

	led->chip = chip;
	led->led_cdev = &led->mcdev.led_cdev;
	led->mcdev.num_colors = count;
	led->mcdev.subled_info = subleds;
	led->mcdev.led_cdev.brightness = brightness;
	led->mcdev.led_cdev.max_brightness = max_brightness;
	led->mcdev.led_cdev.brightness_set_blocking =
		is32fl3207_mc_brightness_set;

	ret = is32fl3207_mc_brightness_set(&led->mcdev.led_cdev,
					   brightness);
	if (ret)
		return ret;

	return devm_led_classdev_multicolor_register_ext(chip->dev, &led->mcdev,
						     &init_data);
}

static int is32fl3207_register_led(struct is32fl3207 *chip,
				   struct fwnode_handle *fwnode,
				   struct is32fl3207_led *led)
{
	unsigned int count = fwnode_get_child_node_count(fwnode);
	bool has_color = fwnode_property_present(fwnode, "color");
	u32 color = LED_COLOR_ID_MAX;
	int ret;

	if (has_color) {
		ret = fwnode_property_read_u32(fwnode, "color", &color);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read color for %pfw\n",
					     fwnode);
	}

	if (color == LED_COLOR_ID_RGB || color == LED_COLOR_ID_MULTI) {
		if (!count)
			return dev_err_probe(chip->dev, -EINVAL,
					     "multicolor LED %pfw has no components\n",
					     fwnode);

		return is32fl3207_register_multicolor(chip, fwnode, led);
	}

	if (count)
		return dev_err_probe(chip->dev, -EINVAL,
				     "single LED %pfw must not have components\n",
				     fwnode);

	return is32fl3207_register_single(chip, fwnode, led);
}

static int is32fl3207_clear_retained_scaling(struct is32fl3207 *chip)
{
	u8 scaling[IS32FL3207_NUM_CHANNELS] = { };
	int ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL, 0);
	if (ret)
		return ret;

	return regmap_bulk_write(chip->regmap,
				 IS32FL3207_REG_SCALING(0), scaling,
				 sizeof(scaling));
}

static int is32fl3207_hw_init(struct is32fl3207 *chip)
{
	u8 scaling[IS32FL3207_NUM_CHANNELS] = { };
	u8 pwm[2 * IS32FL3207_NUM_CHANNELS] = { };
	int disable_ret;
	int ret;

	/* Software reset requires normal operation (SSD = 1). */
	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL,
			   IS32FL3207_CONTROL_ENABLE);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_RESET, 0);
	if (ret)
		return ret;
	usleep_range(200, 300);

	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL, 0);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_GLOBAL_CURRENT,
			   IS32FL3207_GLOBAL_CURRENT_MAX);
	if (ret)
		return ret;

	ret = regmap_bulk_write(chip->regmap, IS32FL3207_REG_SCALING(0),
				scaling, sizeof(scaling));
	if (ret)
		return ret;

	ret = regmap_bulk_write(chip->regmap, IS32FL3207_REG_PWM_LOW(0), pwm,
				sizeof(pwm));
	if (ret)
		return ret;

	/* PWM data can be latched only in normal operation. */
	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL,
			   IS32FL3207_CONTROL_ENABLE);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_PWM_UPDATE, 0);
	disable_ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL, 0);

	return ret ?: disable_ret;
}

static int is32fl3207_enable(struct is32fl3207 *chip)
{
	int ret;

	guard(mutex)(&chip->lock);

	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL,
			   IS32FL3207_CONTROL_ENABLE);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_PWM_UPDATE, 0);
	if (ret)
		regmap_write(chip->regmap, IS32FL3207_REG_CONTROL, 0);

	return ret;
}

static void is32fl3207_disable_locked(struct is32fl3207 *chip)
{
	regmap_write(chip->regmap, IS32FL3207_REG_CONTROL, 0);
	if (chip->enable_gpio)
		gpiod_set_value_cansleep(chip->enable_gpio, 0);
}

static void is32fl3207_disable(void *data)
{
	struct is32fl3207 *chip = data;

	guard(mutex)(&chip->lock);
	chip->shutting_down = true;
	is32fl3207_disable_locked(chip);
}

static const struct regmap_config is32fl3207_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = IS32FL3207_REG_RESET,
};

static int is32fl3207_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct is32fl3207 *ddata;
	unsigned int count;
	unsigned int i = 0;
	u32 riset_ohms;
	int ret;

	count = device_get_child_node_count(dev);
	if (!count || count > IS32FL3207_NUM_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "invalid LED count %u\n", count);

	ddata = devm_kzalloc(dev, struct_size(ddata, leds, count), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	ddata->dev = dev;
	ddata->num_leds = count;
	i2c_set_clientdata(client, ddata);

	ret = device_property_read_u32(dev, "issi,riset-ohms", &riset_ohms);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read RISET value\n");

	if (riset_ohms < 2000)
		return dev_err_probe(dev, -EINVAL,
				     "RISET value %u is below 2000 ohms\n",
				     riset_ohms);

	ddata->output_max_microamp = div_u64(IS32FL3207_CURRENT_NUMERATOR,
					     riset_ohms);
	if (!ddata->output_max_microamp)
		return dev_err_probe(dev, -EINVAL,
				     "RISET value %u is too large\n",
				     riset_ohms);

	ddata->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						     GPIOD_OUT_LOW);
	if (IS_ERR(ddata->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ddata->enable_gpio),
				     "failed to get enable GPIO\n");

	ddata->regmap = devm_regmap_init_i2c(client,
					     &is32fl3207_regmap_config);
	if (IS_ERR(ddata->regmap))
		return dev_err_probe(dev, PTR_ERR(ddata->regmap),
				     "failed to allocate register map\n");

	ret = devm_mutex_init(dev, &ddata->lock);
	if (ret)
		return ret;

	ret = devm_regulator_get_enable_optional(dev, "vcc");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret,
				     "failed to enable VCC regulator\n");

	ret = devm_add_action_or_reset(dev, is32fl3207_disable, ddata);
	if (ret)
		return ret;

	/* Let VCC settle while SDB keeps the outputs disabled. */
	usleep_range(1000, 2000);

	/*
	 * Registers remain accessible with SDB low. Clear retained scaling
	 * before releasing hardware shutdown.
	 */
	ret = is32fl3207_clear_retained_scaling(ddata);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to clear retained current scaling\n");

	if (ddata->enable_gpio)
		gpiod_set_value_cansleep(ddata->enable_gpio, 1);

	/* The SDB rising edge resets the I2C interface; allow it to settle. */
	usleep_range(1000, 2000);

	ret = is32fl3207_hw_init(ddata);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize controller\n");

	device_for_each_child_node_scoped(dev, child) {
		struct is32fl3207_led *led = &ddata->leds[i];

		ret = is32fl3207_register_led(ddata, child, led);
		if (ret)
			return ret;

		i++;
	}

	ret = is32fl3207_enable(ddata);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable controller\n");

	return 0;
}

static void is32fl3207_shutdown(struct i2c_client *client)
{
	struct is32fl3207 *chip = i2c_get_clientdata(client);
	bool retain_state = false;
	unsigned int i;
	int ret = 0;

	guard(mutex)(&chip->lock);
	chip->shutting_down = true;

	for (i = 0; i < chip->num_leds; i++)
		if (chip->leds[i].led_cdev->flags & LED_RETAIN_AT_SHUTDOWN) {
			retain_state = true;
			break;
		}

	if (!retain_state) {
		is32fl3207_disable_locked(chip);
		return;
	}

	for (i = 0; i < chip->num_leds; i++) {
		struct led_classdev *cdev = chip->leds[i].led_cdev;

		if (cdev->flags & LED_RETAIN_AT_SHUTDOWN)
			continue;
		ret = is32fl3207_turn_off_locked(chip, &chip->leds[i]);

		if (ret) {
			dev_warn(chip->dev,
				 "failed to turn off LEDs during shutdown: %d\n",
				 ret);
			break;
		}
	}
}

static const struct of_device_id is32fl3207_of_match[] = {
	{ .compatible = "issi,is32fl3207" },
	{ }
};
MODULE_DEVICE_TABLE(of, is32fl3207_of_match);

static const struct i2c_device_id is32fl3207_id[] = {
	{ .name = "is32fl3207" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, is32fl3207_id);

static struct i2c_driver is32fl3207_driver = {
	.driver = {
		.name = "is32fl3207",
		.of_match_table = is32fl3207_of_match,
	},
	.probe = is32fl3207_probe,
	.shutdown = is32fl3207_shutdown,
	.id_table = is32fl3207_id,
};
module_i2c_driver(is32fl3207_driver);

MODULE_AUTHOR("Ahmad Byagowi <ahmadexp@gmail.com>");
MODULE_DESCRIPTION("Lumissil IS32FL3207 LED controller driver");
MODULE_LICENSE("GPL");
