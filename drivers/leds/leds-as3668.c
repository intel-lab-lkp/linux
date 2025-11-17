// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Osram AMS AS3668 LED Driver IC
 *
 *  Copyright (C) 2025 Lukas Timmermann <linux@timmermann.space>
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/uleds.h>

#define AS3668_MAX_LEDS			4

/* Chip Ident */

#define AS3668_CHIP_ID1_REG		0x3e
#define AS3668_CHIP_ID			0xa5

/* Current Control */

#define AS3668_CURR_MODE_REG		0x01
#define AS3668_CURR_MODE_OFF		0x0
#define AS3668_CURR_MODE_ON		0x1
#define AS3668_CURR1_MODE_MASK		GENMASK(1, 0)
#define AS3668_CURR2_MODE_MASK		GENMASK(3, 2)
#define AS3668_CURR3_MODE_MASK		GENMASK(5, 4)
#define AS3668_CURR4_MODE_MASK		GENMASK(7, 6)
#define AS3668_CURR1_REG		0x02
#define AS3668_CURR2_REG		0x03
#define AS3668_CURR3_REG		0x04
#define AS3668_CURR4_REG		0x05

struct as3668_led {
	struct led_classdev cdev;
	struct as3668 *chip;
	struct fwnode_handle *fwnode;
	int led_id;
};

struct as3668 {
	struct i2c_client *client;
	struct as3668_led leds[AS3668_MAX_LEDS];
};

static void as3668_channel_mode_set(struct as3668 *as3668, int led_id, u8 mode)
{
	int err;
	u8 reg;

	reg = i2c_smbus_read_byte_data(as3668->client, AS3668_CURR_MODE_REG);
	if (reg < 0) {
		dev_err(&as3668->client->dev, "failed to read channel modes\n");
		return;
	}

	switch (led_id) {
	case 0:
		reg &= ~AS3668_CURR1_MODE_MASK;
		reg |= FIELD_PREP(AS3668_CURR1_MODE_MASK, mode);
		break;
	case 1:
		reg &= ~AS3668_CURR2_MODE_MASK;
		reg |= FIELD_PREP(AS3668_CURR2_MODE_MASK, mode);
		break;
	case 2:
		reg &= ~AS3668_CURR3_MODE_MASK;
		reg |= FIELD_PREP(AS3668_CURR3_MODE_MASK, mode);
		break;
	case 3:
		reg &= ~AS3668_CURR4_MODE_MASK;
		reg |= FIELD_PREP(AS3668_CURR4_MODE_MASK, mode);
		break;
	default:
		return;
	}

	err = i2c_smbus_write_byte_data(as3668->client, AS3668_CURR_MODE_REG, reg);
	if (err)
		dev_err(&as3668->client->dev, "failed to set channel modes\n");
}

static enum led_brightness as3668_brightness_get(struct led_classdev *cdev)
{
	struct as3668_led *led = container_of(cdev, struct as3668_led, cdev);

	return i2c_smbus_read_byte_data(led->chip->client, AS3668_CURR1_REG + led->led_id);
}

static void as3668_brightness_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct as3668_led *led = container_of(cdev, struct as3668_led, cdev);
	int err;

	if (brightness == 0)
		as3668_channel_mode_set(led->chip, led->led_id, AS3668_CURR_MODE_OFF);
	else
		as3668_channel_mode_set(led->chip, led->led_id, AS3668_CURR_MODE_ON);

	err = i2c_smbus_write_byte_data(led->chip->client,
					AS3668_CURR1_REG + led->led_id,
					brightness);

	if (err)
		dev_err(&led->chip->client->dev, "failed to set brightness: %d\n", err);
}

static int as3668_dt_init(struct as3668 *as3668)
{
	struct device *dev = &as3668->client->dev;
	struct as3668_led *led;
	struct led_init_data init_data = {};
	int err;
	u32 reg;

	for_each_available_child_of_node_scoped(dev_of_node(dev), child) {
		err = of_property_read_u32(child, "reg", &reg);
		if (err)
			return dev_err_probe(dev, err, "failed to read 'reg' property");

		if (reg < 0 || reg > AS3668_MAX_LEDS)
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "unsupported LED: %d\n", reg);

		led = &as3668->leds[reg];
		led->fwnode = of_fwnode_handle(child);

		led->led_id = reg;
		led->chip = as3668;

		led->cdev.max_brightness = U8_MAX;
		led->cdev.brightness_get = as3668_brightness_get;
		led->cdev.brightness_set = as3668_brightness_set;

		init_data.fwnode = led->fwnode;
		init_data.default_label = ":";

		err = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
		if (err)
			return dev_err_probe(dev, err, "failed to register LED %d\n", reg);
	}

	return 0;
}

static int as3668_probe(struct i2c_client *client)
{
	struct as3668 *as3668;
	int err;
	u8 chip_id;

	chip_id = i2c_smbus_read_byte_data(client, AS3668_CHIP_ID1_REG);
	if (chip_id != AS3668_CHIP_ID)
		return dev_err_probe(&client->dev, -ENODEV,
				     "expected chip ID 0x%02x, got 0x%02x\n",
				     AS3668_CHIP_ID, chip_id);

	as3668 = devm_kzalloc(&client->dev, sizeof(*as3668), GFP_KERNEL);
	if (!as3668)
		return -ENOMEM;

	as3668->client = client;

	err = as3668_dt_init(as3668);
	if (err)
		return err;

	/* Set all four channel modes to 'off' */
	err = i2c_smbus_write_byte_data(client, AS3668_CURR_MODE_REG,
					FIELD_PREP(AS3668_CURR1_MODE_MASK, AS3668_CURR_MODE_OFF) |
					FIELD_PREP(AS3668_CURR2_MODE_MASK, AS3668_CURR_MODE_OFF) |
					FIELD_PREP(AS3668_CURR3_MODE_MASK, AS3668_CURR_MODE_OFF) |
					FIELD_PREP(AS3668_CURR4_MODE_MASK, AS3668_CURR_MODE_OFF));

	/* Set initial currents to 0mA */
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR1_REG, 0);
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR2_REG, 0);
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR3_REG, 0);
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR4_REG, 0);

	if (err)
		return dev_err_probe(&client->dev, -EIO, "failed to write to the device\n");

	return 0;
}

static void as3668_remove(struct i2c_client *client)
{
	int err;

	err = i2c_smbus_write_byte_data(client, AS3668_CURR_MODE_REG, 0);
	if (err)
		dev_err(&client->dev, "failed to turn off the LEDs\n");
}

static const struct i2c_device_id as3668_idtable[] = {
	{ "as3668" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, as3668_idtable);

static const struct of_device_id as3668_match_table[] = {
	{ .compatible = "ams,as3668" },
	{ }
};
MODULE_DEVICE_TABLE(of, as3668_match_table);

static struct i2c_driver as3668_driver = {
	.driver = {
		.name = "leds_as3668",
		.of_match_table = as3668_match_table,
	},
	.probe = as3668_probe,
	.remove = as3668_remove,
	.id_table = as3668_idtable,
};
module_i2c_driver(as3668_driver);

MODULE_AUTHOR("Lukas Timmermann <linux@timmermann.space>");
MODULE_DESCRIPTION("AS3668 LED driver");
MODULE_LICENSE("GPL");
