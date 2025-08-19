// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight driver for Maxim MAX25014
 *
 * Copyright (C) 2025 GOcontroll B.V.
 * Author: Maud Spierings <maudspierings@gocontroll.com>
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define MAX25014_ISET_DEFAULT_100 11
#define MAX_BRIGHTNESS (100)
#define MIN_BRIGHTNESS (0)
#define TON_MAX (130720) /* @153Hz */
#define TON_STEP (1307) /* @153Hz */
#define TON_MIN (0)

#define MAX25014_DEV_ID         (0x00)
#define MAX25014_REV_ID         (0x01)
#define MAX25014_ISET           (0x02)
#define MAX25014_IMODE          (0x03)
#define MAX25014_TON1H          (0x04)
#define MAX25014_TON1L          (0x05)
#define MAX25014_TON2H          (0x06)
#define MAX25014_TON2L          (0x07)
#define MAX25014_TON3H          (0x08)
#define MAX25014_TON3L          (0x09)
#define MAX25014_TON4H          (0x0A)
#define MAX25014_TON4L          (0x0B)
#define MAX25014_TON_1_4_LSB    (0x0C)
#define MAX25014_SETTING        (0x12)
#define MAX25014_DISABLE        (0x13)
#define MAX25014_BSTMON         (0x14)
#define MAX25014_IOUT1          (0x15)
#define MAX25014_IOUT2          (0x16)
#define MAX25014_IOUT3          (0x17)
#define MAX25014_IOUT4          (0x18)
#define MAX25014_OPEN           (0x1B)
#define MAX25014_SHORT_GND      (0x1C)
#define MAX25014_SHORT_LED      (0x1D)
#define MAX25014_MASK           (0x1E)
#define MAX25014_DIAG           (0x1F)

#define MAX25014_IMODE_HDIM     BIT(2)
#define MAX25014_ISET_ENABLE    BIT(5)
#define MAX25014_ISET_PSEN      BIT(4)
#define MAX25014_DIAG_HW_RST    BIT(2)
#define MAX25014_SETTING_FPWM   GENMASK(6, 4)

struct max25014 {
	struct i2c_client *client;
	struct backlight_device *bl;
	struct regmap *regmap;
	struct max25014_platform_data *pdata;
	struct gpio_desc *enable;
	struct regulator *vin; /* regulator for boost converter Vin rail */
	uint32_t initial_brightness;
	uint32_t iset;
	uint8_t strings_mask;
};

static const struct regmap_config max25014_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX25014_DIAG,
};

/**
 * @brief control the brightness with i2c registers
 *
 * @param regmap trivial
 * @param brt brightness
 * @return int
 */
static int max25014_register_control(struct regmap *regmap, uint32_t brt)
{
	uint32_t reg = TON_STEP * brt;
	int ret;
	/*
	 * 18 bit number lowest, 2 bits in first register,
	 * next lowest 8 in the L register, next 8 in the H register
	 * Seemingly setting the strength of only one string controls all of
	 * them, individual settings don't affect the outcome.
	 */

	ret = regmap_write(regmap, MAX25014_TON_1_4_LSB, reg & 0b00000011);
	if (ret != 0)
		return ret;
	ret = regmap_write(regmap, MAX25014_TON1L, (reg >> 2) & 0b11111111);
	if (ret != 0)
		return ret;
	return regmap_write(regmap, MAX25014_TON1H, (reg >> 10) & 0b11111111);
}

static int max25014_check_errors(struct max25014 *maxim)
{
	uint8_t i;
	int ret;
	uint32_t val;

	ret = regmap_read(maxim->regmap, MAX25014_OPEN, &val);
	if (ret != 0)
		return ret;
	if (val > 0) {
		dev_err(&maxim->client->dev, "Open led strings detected on:\n");
		for (i = 0; i < 4; i++) {
			if (val & 1 << i)
				dev_err(&maxim->client->dev, "string %d\n", i + 1);
		}
		return -EIO;
	}

	ret = regmap_read(maxim->regmap, MAX25014_SHORT_GND, &val);
	if (ret != 0)
		return ret;
	if (val > 0) {
		dev_err(&maxim->client->dev, "Short to ground detected on:\n");
		for (i = 0; i < 4; i++) {
			if (val & 1 << i)
				dev_err(&maxim->client->dev, "string %d\n", i + 1);
		}
		return -EIO;
	}

	ret = regmap_read(maxim->regmap, MAX25014_SHORT_GND, &val);
	if (ret != 0)
		return ret;
	if (val > 0) {
		dev_err(&maxim->client->dev, "Shorted led detected on:\n");
		for (i = 0; i < 4; i++) {
			if (val & 1 << i)
				dev_err(&maxim->client->dev, "string %d\n", i + 1);
		}
		return -EIO;
	}

	ret = regmap_read(maxim->regmap, MAX25014_DIAG, &val);
	if (ret != 0)
		return ret;
	/*
	 * The HW_RST bit always starts at 1 after power up.
	 * It is reset on first read, does not indicate an error.
	 */
	if (val > 0 && val != MAX25014_DIAG_HW_RST) {
		if (val & 0b1)
			dev_err(&maxim->client->dev, "Overtemperature shutdown\n");
		if (val & 0b10)
			dev_warn(&maxim->client->dev,
				 "Chip is getting too hot (>125C)\n");
		if (val & 0b1000)
			dev_err(&maxim->client->dev, "Boost converter overvoltage\n");
		if (val & 0b10000)
			dev_err(&maxim->client->dev, "Boost converter undervoltage\n");
		if (val & 0b100000)
			dev_err(&maxim->client->dev, "IREF out of range\n");
		return -EIO;
	}
	return 0;
}

/*
 * 1. disable unused strings
 * 2. set dim mode
 * 3. set initial brightness
 * 4. set setting register
 * 5. enable the backlight
 */
static int max25014_configure(struct max25014 *maxim)
{
	int ret;
	uint32_t val;

	ret = regmap_write(maxim->regmap, MAX25014_DISABLE,
			   maxim->strings_mask);
	if (ret != 0)
		return ret;

	ret = regmap_write(maxim->regmap, MAX25014_IMODE, MAX25014_IMODE_HDIM);
	if (ret != 0)
		return ret;

	max25014_register_control(maxim->regmap,
				  maxim->initial_brightness);

	ret = regmap_read(maxim->regmap, MAX25014_SETTING, &val);
	if (ret != 0)
		return ret;

	ret = regmap_write(
		maxim->regmap, MAX25014_SETTING,
		val & ~MAX25014_SETTING_FPWM);
	if (ret != 0)
		return ret;

	ret = regmap_write(maxim->regmap, MAX25014_ISET,
			   maxim->iset | MAX25014_ISET_ENABLE | MAX25014_ISET_PSEN);
	return ret;
}

static int max25014_update_status(struct backlight_device *bl_dev)
{
	struct max25014 *maxim = bl_get_data(bl_dev);

	if (bl_dev->props.state & BL_CORE_SUSPENDED)
		bl_dev->props.brightness = 0;

	return max25014_register_control(maxim->regmap, bl_dev->props.brightness);
}

static const struct backlight_ops max25014_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = max25014_update_status,
};

static int max25014_parse_dt(struct max25014 *maxim)
{
	struct device *dev = &maxim->client->dev;
	struct device_node *node = dev->of_node;
	uint32_t strings[4];
	int res, i;

	if (!node) {
		dev_err(dev, "no platform data\n");
		return -EINVAL;
	}

	res = of_property_count_u32_elems(node, "maxim,strings");
	if (res == 4) {
		of_property_read_u32_array(node, "maxim,strings", strings, 4);
	} else {
		dev_err(dev, "strings property not correctly defined\n");
		return -EINVAL;
	}

	for (i = 0; i < 4; i++) {
		if (strings[i] == 0)
			maxim->strings_mask |= 1 << i;
	}

	maxim->initial_brightness = 50U;
	of_property_read_u32(node, "default-brightness", &maxim->initial_brightness);
	maxim->iset = MAX25014_ISET_DEFAULT_100;
	of_property_read_u32(node, "maxim,iset", &maxim->iset);

	if (maxim->iset < 0 || maxim->iset > 15) {
		dev_err(dev,
			"Invalid iset, should be a value from 0-15, entered was %d\n",
			maxim->iset);
		return -EINVAL;
	}

	if (maxim->initial_brightness < 0 || maxim->initial_brightness > 100) {
		dev_err(dev,
			"Invalid initial brightness, should be a value from 0-100, entered was %d\n",
			maxim->initial_brightness);
		return -EINVAL;
	}

	return 0;
}

static int max25014_probe(struct i2c_client *cl)
{
	struct backlight_device *bl;
	const struct i2c_device_id *id = i2c_client_get_device_id(cl);
	struct max25014 *maxim;
	struct backlight_properties props;
	int ret;

	maxim = devm_kzalloc(&cl->dev, sizeof(struct max25014), GFP_KERNEL);
	if (!maxim)
		return -ENOMEM;

	maxim->client = cl;

	ret = max25014_parse_dt(maxim);
	if (ret < 0)
		return ret;

	maxim->vin = devm_regulator_get(&maxim->client->dev, "power");
	if (IS_ERR(maxim->vin)) {
		if (PTR_ERR(maxim->vin) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		maxim->vin = NULL;
	}

	if (maxim->vin) {
		ret = regulator_enable(maxim->vin);
		if (ret < 0) {
			dev_err(&maxim->client->dev, "failed to enable Vin: %d\n", ret);
			return ret;
		}
	}

	maxim->enable =
		devm_gpiod_get_optional(&maxim->client->dev, "enable", GPIOD_ASIS);
	if (IS_ERR(maxim->enable)) {
		ret = PTR_ERR(maxim->enable);
		dev_err(&maxim->client->dev, "failed to get enable gpio: %d\n", ret);
		goto disable_vin;
	}

	if (maxim->enable) {
		gpiod_set_value_cansleep(maxim->enable, 1);

		/* Datasheet Electrical Characteristics tSTARTUP 2ms */
		usleep_range(2000, 2500);
	}

	maxim->regmap = devm_regmap_init_i2c(cl, &max25014_regmap_config);
	if (IS_ERR(maxim->regmap)) {
		ret = PTR_ERR(maxim->regmap);
		dev_err(&maxim->client->dev, "failed to initialize the i2c regmap: %d\n", ret);
		goto disable_full;
	}

	i2c_set_clientdata(cl, maxim);

	ret = max25014_check_errors(maxim);
	if (ret) { /* error is already reported in the above function */
		goto disable_full;
	}

	ret = max25014_configure(maxim);
	if (ret) {
		dev_err(&maxim->client->dev, "device config err: %d", ret);
		goto disable_full;
	}

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = MAX_BRIGHTNESS;

	props.brightness = maxim->initial_brightness;

	bl = devm_backlight_device_register(&maxim->client->dev, id->name, &maxim->client->dev,
					    maxim, &max25014_bl_ops, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);

	maxim->bl = bl;

	return 0;

disable_full:
	if (maxim->enable)
		gpiod_set_value_cansleep(maxim->enable, 0);
disable_vin:
	if (maxim->vin)
		regulator_disable(maxim->vin);
	return ret;
}

static void max25014_remove(struct i2c_client *cl)
{
	struct max25014 *maxim = i2c_get_clientdata(cl);

	maxim->bl->props.brightness = 0;
	max25014_update_status(maxim->bl);
	if (maxim->enable)
		gpiod_set_value_cansleep(maxim->enable, 0);
	if (maxim->vin)
		regulator_disable(maxim->vin);
}

static const struct of_device_id max25014_dt_ids[] = {
	{ .compatible = "maxim,max25014", },
	{ }
};
MODULE_DEVICE_TABLE(of, max25014_dt_ids);

static const struct i2c_device_id max25014_ids[] = {
	{ "max25014" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max25014_ids);

static struct i2c_driver max25014_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_match_ptr(max25014_dt_ids),
	},
	.probe = max25014_probe,
	.remove = max25014_remove,
	.id_table = max25014_ids,
};
module_i2c_driver(max25014_driver);

MODULE_DESCRIPTION("Maxim MAX25014 backlight driver");
MODULE_AUTHOR("Maud Spierings <maudspierings@gocontroll.com>");
MODULE_LICENSE("GPL");
