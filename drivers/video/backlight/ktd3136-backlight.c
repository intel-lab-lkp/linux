// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kinetic Technologies KTD3136 3-Channel LED Backlight Driver
 *
 * Copyright (C) 2026
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#define KTD3136_REG_DEV_ID		0x00
#define KTD3136_REG_SW_RESET		0x01
#define KTD3136_REG_MODE		0x02
#define KTD3136_REG_CONTROL		0x03
#define KTD3136_REG_RATIO_LSB		0x04
#define KTD3136_REG_RATIO_MSB		0x05
#define KTD3136_REG_PWM			0x06
#define KTD3136_REG_STATUS		0x0A

#define KTD3136_DEV_ID_VAL		0x18
#define KTD3136_DEV_ID_VAL2		0x19

#define KTD3136_MODE_ON			0xC9	/* Boost ON, 3 channels enabled */
#define KTD3136_MODE_STANDBY		0x98	/* Standby mode */
#define KTD3136_CONTROL_LINEAR		0x02	/* Linear dimming mapping */
#define KTD3136_PWM_DEFAULT		0x1B	/* Default PWM/boost frequency */

#define KTD3136_DEFAULT_MAX_BRIGHTNESS	2047	/* 11-bit resolution */
#define KTD3136_DEFAULT_BRIGHTNESS	2047

struct ktd3136_data {
	struct i2c_client *client;
	struct backlight_device *bd;
	struct gpio_desc *enable_gpio;
	struct regulator *vin;
	/* Protects chip registers and state */
	struct mutex lock;
	bool is_enabled;
};

static int ktd3136_write(struct ktd3136_data *chip, u8 reg, u8 val)
{
	int ret;

	ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	if (ret < 0)
		dev_err(&chip->client->dev, "failed to write reg 0x%02x: %d\n", reg, ret);

	return ret;
}

static int ktd3136_read(struct ktd3136_data *chip, u8 reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(chip->client, reg);
	if (ret < 0)
		dev_err(&chip->client->dev, "failed to read reg 0x%02x: %d\n", reg, ret);

	return ret;
}

static int ktd3136_power_on(struct ktd3136_data *chip)
{
	int ret;

	if (chip->is_enabled)
		return 0;

	if (chip->enable_gpio) {
		gpiod_set_value_cansleep(chip->enable_gpio, 1);
		/* Allow oscillator and internal logic to stabilize */
		usleep_range(2000, 3000);
	}

	ret = ktd3136_write(chip, KTD3136_REG_CONTROL, KTD3136_CONTROL_LINEAR);
	if (ret < 0)
		goto err_off;

	ret = ktd3136_write(chip, KTD3136_REG_PWM, KTD3136_PWM_DEFAULT);
	if (ret < 0)
		goto err_off;

	chip->is_enabled = true;
	return 0;

err_off:
	if (chip->enable_gpio)
		gpiod_set_value_cansleep(chip->enable_gpio, 0);
	return ret;
}

static void ktd3136_power_off(struct ktd3136_data *chip)
{
	if (!chip->is_enabled)
		return;

	ktd3136_write(chip, KTD3136_REG_MODE, KTD3136_MODE_STANDBY);

	if (chip->enable_gpio)
		gpiod_set_value_cansleep(chip->enable_gpio, 0);

	chip->is_enabled = false;
}

static int ktd3136_update_status(struct backlight_device *bd)
{
	struct ktd3136_data *chip = bl_get_data(bd);
	int brightness = backlight_get_brightness(bd);
	int ret = 0;
	u8 lsb, msb;

	mutex_lock(&chip->lock);

	if (backlight_is_blank(bd) || brightness == 0) {
		ktd3136_power_off(chip);
		goto out;
	}

	ret = ktd3136_power_on(chip);
	if (ret < 0)
		goto out;

	lsb = brightness & 0x07;
	msb = (brightness >> 3) & 0xFF;

	ret = ktd3136_write(chip, KTD3136_REG_RATIO_LSB, lsb);
	if (ret < 0)
		goto out;

	ret = ktd3136_write(chip, KTD3136_REG_RATIO_MSB, msb);
	if (ret < 0)
		goto out;

	ret = ktd3136_write(chip, KTD3136_REG_MODE, KTD3136_MODE_ON);

out:
	mutex_unlock(&chip->lock);
	return ret;
}

static const struct backlight_ops ktd3136_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = ktd3136_update_status,
};

static int ktd3136_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct backlight_properties props;
	struct ktd3136_data *chip;
	int ret, val;
	u32 def_brightness = KTD3136_DEFAULT_BRIGHTNESS;
	u32 max_brightness = KTD3136_DEFAULT_MAX_BRIGHTNESS;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	mutex_init(&chip->lock);

	chip->vin = devm_regulator_get_optional(dev, "vin");
	if (IS_ERR(chip->vin)) {
		ret = PTR_ERR(chip->vin);
		if (ret != -ENODEV)
			return dev_err_probe(dev, ret, "Failed to get vin regulator\n");
		chip->vin = NULL;
	}

	if (chip->vin) {
		ret = regulator_enable(chip->vin);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to enable vin regulator\n");
	}

	chip->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(chip->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(chip->enable_gpio),
				     "Failed to get enable GPIO\n");

	if (chip->enable_gpio)
		usleep_range(2000, 3000);

	val = ktd3136_read(chip, KTD3136_REG_DEV_ID);
	if (val < 0)
		return dev_err_probe(dev, val, "Failed to read device ID\n");

	if (val != KTD3136_DEV_ID_VAL && val != KTD3136_DEV_ID_VAL2) {
		dev_err(dev, "Unknown device ID: 0x%02x\n", val);
		return -ENODEV;
	}

	dev_info(dev, "Kinetic KTD3136 detected (ID: 0x%02x)\n", val);

	device_property_read_u32(dev, "max-brightness", &max_brightness);
	if (max_brightness > KTD3136_DEFAULT_MAX_BRIGHTNESS)
		max_brightness = KTD3136_DEFAULT_MAX_BRIGHTNESS;

	device_property_read_u32(dev, "default-brightness", &def_brightness);
	if (def_brightness > max_brightness)
		def_brightness = max_brightness;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = max_brightness;
	props.brightness = def_brightness;

	chip->bd = devm_backlight_device_register(dev, dev_name(dev), dev, chip,
						  &ktd3136_backlight_ops, &props);
	if (IS_ERR(chip->bd))
		return dev_err_probe(dev, PTR_ERR(chip->bd),
				     "Failed to register backlight device\n");

	backlight_update_status(chip->bd);

	return 0;
}

static void ktd3136_remove(struct i2c_client *client)
{
	struct ktd3136_data *chip = i2c_get_clientdata(client);

	if (chip)
		ktd3136_power_off(chip);
}

static const struct of_device_id ktd3136_of_match[] = {
	{ .compatible = "kinetic,ktd3136" },
	{ }
};
MODULE_DEVICE_TABLE(of, ktd3136_of_match);

static const struct i2c_device_id ktd3136_i2c_id[] = {
	{ "ktd3136", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ktd3136_i2c_id);

static struct i2c_driver ktd3136_driver = {
	.driver = {
		.name = "ktd3136-backlight",
		.of_match_table = ktd3136_of_match,
	},
	.probe = ktd3136_probe,
	.remove = ktd3136_remove,
	.id_table = ktd3136_i2c_id,
};
module_i2c_driver(ktd3136_driver);

MODULE_AUTHOR("Antigravity Team");
MODULE_DESCRIPTION("Kinetic Technologies KTD3136 Backlight Driver");
MODULE_LICENSE("GPL");
