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
	struct is32fl3207_led leds[] __counted_by(num_leds);
};

static int is32fl3207_parse_led_properties(struct is32fl3207 *chip,
					   struct fwnode_handle *fwnode,
					   unsigned int *max_brightness)
{
	const char *default_state;
	u32 value;
	int ret;

	if (fwnode_property_present(fwnode, "retain-state-shutdown"))
		return dev_err_probe(chip->dev, -EINVAL,
				     "retain-state-shutdown is not supported for %pfw\n",
				     fwnode);

	if (fwnode_property_present(fwnode, "default-brightness"))
		return dev_err_probe(chip->dev, -EINVAL,
				     "default-brightness is not supported for %pfw\n",
				     fwnode);

	if (fwnode_property_present(fwnode, "default-state")) {
		ret = fwnode_property_read_string(fwnode, "default-state",
						  &default_state);
		if (ret)
			return dev_err_probe(chip->dev, ret,
					     "failed to read default state for %pfw\n",
					     fwnode);
		if (strcmp(default_state, "off"))
			return dev_err_probe(chip->dev, -EINVAL,
					     "default state %s is not supported for %pfw\n",
					     default_state, fwnode);
	}

	*max_brightness = IS32FL3207_MAX_BRIGHTNESS;
	if (!fwnode_property_present(fwnode, "max-brightness"))
		return 0;

	ret = fwnode_property_read_u32(fwnode, "max-brightness", &value);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read maximum brightness for %pfw\n",
				     fwnode);
	if (!value || value > IS32FL3207_MAX_BRIGHTNESS)
		return dev_err_probe(chip->dev, -EINVAL,
				     "invalid maximum brightness %u for %pfw\n",
				     value, fwnode);

	*max_brightness = value;
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

static int is32fl3207_write_channels(struct is32fl3207 *chip,
				     const struct mc_subled *subleds,
				     unsigned int num_channels)
{
	unsigned int i;
	int ret;

	guard(mutex)(&chip->lock);

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

	return is32fl3207_write_channels(led->chip, &subled, 1);
}

static int is32fl3207_mc_brightness_set(struct led_classdev *cdev,
					enum led_brightness brightness)
{
	struct led_classdev_mc *mcdev = lcdev_to_mccdev(cdev);
	struct is32fl3207_led *led = container_of(mcdev, struct is32fl3207_led,
						     mcdev);

	led_mc_calc_color_components(mcdev, brightness);

	return is32fl3207_write_channels(led->chip, mcdev->subled_info,
					    mcdev->num_colors);
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
				     "failed to read channel for %pfw\n", fwnode);

	if (reg >= IS32FL3207_NUM_CHANNELS)
		return dev_err_probe(chip->dev, -EINVAL,
				     "channel %u is out of range\n", reg);

	if (test_bit(reg, chip->channels))
		return dev_err_probe(chip->dev, -EINVAL,
				     "channel %u is used more than once\n", reg);

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
	u32 color;
	int ret;

	ret = is32fl3207_parse_led_properties(chip, fwnode,
					      &max_brightness);
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
					     "invalid single LED color %u\n", color);
	}

	ret = is32fl3207_configure_channel(chip, fwnode, &led->channel);
	if (ret)
		return ret;

	led->chip = chip;
	led->cdev.max_brightness = max_brightness;
	led->cdev.brightness_set_blocking = is32fl3207_brightness_set;

	return devm_led_classdev_register_ext(chip->dev, &led->cdev, &init_data);
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
	unsigned int count;
	unsigned int i = 0;
	u32 group_color;
	u32 group_reg;
	unsigned int first_channel = IS32FL3207_NUM_CHANNELS;
	int ret;

	ret = is32fl3207_parse_led_properties(chip, fwnode,
					      &max_brightness);
	if (ret)
		return ret;

	ret = fwnode_property_read_u32(fwnode, "color", &group_color);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read color for %pfw\n", fwnode);

	if (group_color != LED_COLOR_ID_RGB &&
	    group_color != LED_COLOR_ID_MULTI)
		return dev_err_probe(chip->dev, -EINVAL,
				     "invalid multicolor LED color %u\n", group_color);

	ret = fwnode_property_read_u32(fwnode, "reg", &group_reg);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read group index for %pfw\n", fwnode);

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
					     "invalid component color %u\n", color);
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

	if (group_reg != first_channel)
		return dev_err_probe(chip->dev, -EINVAL,
				     "group index %u does not match first channel %u\n",
				     group_reg, first_channel);

	led->chip = chip;
	led->mcdev.num_colors = count;
	led->mcdev.subled_info = subleds;
	led->mcdev.led_cdev.max_brightness = max_brightness;
	led->mcdev.led_cdev.brightness_set_blocking =
		is32fl3207_mc_brightness_set;

	return devm_led_classdev_multicolor_register_ext(chip->dev, &led->mcdev,
						     &init_data);
}

static int is32fl3207_hw_init(struct is32fl3207 *chip)
{
	u8 scaling[IS32FL3207_NUM_CHANNELS];
	u8 pwm[2 * IS32FL3207_NUM_CHANNELS] = { };
	int ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL,
			   IS32FL3207_CONTROL_ENABLE);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_RESET, 0);
	if (ret)
		return ret;
	usleep_range(200, 300);

	ret = regmap_write(chip->regmap, IS32FL3207_REG_CONTROL,
			   IS32FL3207_CONTROL_ENABLE);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, IS32FL3207_REG_GLOBAL_CURRENT,
			   IS32FL3207_GLOBAL_CURRENT_MAX);
	if (ret)
		return ret;

	memset(scaling, 0xff, sizeof(scaling));
	ret = regmap_bulk_write(chip->regmap, IS32FL3207_REG_SCALING(0),
				scaling, sizeof(scaling));
	if (ret)
		return ret;

	ret = regmap_bulk_write(chip->regmap, IS32FL3207_REG_PWM_LOW(0), pwm,
				sizeof(pwm));
	if (ret)
		return ret;

	return regmap_write(chip->regmap, IS32FL3207_REG_PWM_UPDATE, 0);
}

static void is32fl3207_disable(void *data)
{
	struct is32fl3207 *chip = data;

	guard(mutex)(&chip->lock);
	regmap_write(chip->regmap, IS32FL3207_REG_CONTROL, 0);
	if (chip->enable_gpio)
		gpiod_set_value_cansleep(chip->enable_gpio, 0);
}

static const struct regmap_config is32fl3207_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = IS32FL3207_REG_RESET,
};

static int is32fl3207_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct is32fl3207 *chip;
	unsigned int count;
	unsigned int i = 0;
	u32 riset_ohms;
	int ret;

	count = device_get_child_node_count(dev);
	if (!count || count > IS32FL3207_NUM_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "invalid LED count %u\n", count);

	chip = devm_kzalloc(dev, struct_size(chip, leds, count), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;
	chip->num_leds = count;
	i2c_set_clientdata(client, chip);

	ret = device_property_read_u32(dev, "issi,riset-ohms", &riset_ohms);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read RISET value\n");

	if (riset_ohms < 2000)
		return dev_err_probe(dev, -EINVAL,
				     "RISET value %u is below 2000 ohms\n",
				     riset_ohms);

	chip->output_max_microamp = div_u64(IS32FL3207_CURRENT_NUMERATOR,
					    riset_ohms);
	if (!chip->output_max_microamp)
		return dev_err_probe(dev, -EINVAL,
				     "RISET value %u is too large\n", riset_ohms);

	ret = devm_regulator_get_enable_optional(dev, "vcc");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to enable VCC regulator\n");

	chip->enable_gpio = devm_gpiod_get_optional(dev, "enable",
						    GPIOD_OUT_HIGH);
	if (IS_ERR(chip->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(chip->enable_gpio),
				     "failed to get enable GPIO\n");

	if (chip->enable_gpio)
		usleep_range(1000, 2000);

	chip->regmap = devm_regmap_init_i2c(client, &is32fl3207_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(dev, PTR_ERR(chip->regmap),
				     "failed to allocate register map\n");

	ret = devm_mutex_init(dev, &chip->lock);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, is32fl3207_disable, chip);
	if (ret)
		return ret;

	ret = is32fl3207_hw_init(chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize controller\n");

	device_for_each_child_node_scoped(dev, child) {
		struct is32fl3207_led *led = &chip->leds[i];

		if (fwnode_get_child_node_count(child))
			ret = is32fl3207_register_multicolor(chip, child, led);
		else
			ret = is32fl3207_register_single(chip, child, led);
		if (ret)
			return ret;

		i++;
	}

	return 0;
}

static void is32fl3207_shutdown(struct i2c_client *client)
{
	is32fl3207_disable(i2c_get_clientdata(client));
}

static const struct of_device_id is32fl3207_of_match[] = {
	{ .compatible = "issi,is32fl3207" },
	{ }
};
MODULE_DEVICE_TABLE(of, is32fl3207_of_match);

static const struct i2c_device_id is32fl3207_id[] = {
	{ "is32fl3207" },
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
