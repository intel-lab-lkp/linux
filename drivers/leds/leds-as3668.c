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

#define AS3668_MAX_LEDS 4

/* Chip Registers */
#define AS3668_CHIP_ID1 0x3e
#define AS3668_CHIP_ID2 0x3f

#define AS3668_CHIP_ID2_SERIAL_MASK GENMASK(7, 4)
#define AS3668_CHIP_ID2_REV_MASK GENMASK(3, 0)

#define AS3668_CURRX_CONTROL 0x01
#define AS3668_CURR1 0x02
#define AS3668_CURR2 0x03
#define AS3668_CURR3 0x04
#define AS3668_CURR4 0x05

/* Constants */
#define AS3668_CHIP_IDENT 0xa5
#define AS3668_CHIP_REV1 0x01

struct as3668_led {
	struct led_classdev cdev;
	struct as3668 *chip;
	struct fwnode_handle *fwnode;

	int num;
};

struct as3668 {
	struct i2c_client *client;
	struct as3668_led leds[AS3668_MAX_LEDS];
};

static int as3668_read_value(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

static int as3668_write_value(struct i2c_client *client, u8 reg, u8 value)
{
	int err = i2c_smbus_write_byte_data(client, reg, value);

	if (err)
		dev_err(&client->dev, "error writing to reg 0x%02x, returned %d\n", reg, err);

	return err;
}

static enum led_brightness as3668_brightness_get(struct led_classdev *cdev)
{
	struct as3668_led *led = container_of(cdev, struct as3668_led, cdev);

	return as3668_read_value(led->chip->client, AS3668_CURR1 + led->num);
}

static void as3668_brightness_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct as3668_led *led = container_of(cdev, struct as3668_led, cdev);

	as3668_write_value(led->chip->client, AS3668_CURR1 + led->num, brightness);
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
		if (err) {
			dev_err(dev, "unable to read device tree led reg, err %d\n", err);
			return err;
		}

		if (reg < 0 || reg > AS3668_MAX_LEDS) {
			dev_err(dev, "unsupported led reg %d\n", reg);
			return -EOPNOTSUPP;
		}

		led = &as3668->leds[reg];
		led->fwnode = of_fwnode_handle(child);

		led->num = reg;
		led->chip = as3668;

		led->cdev.max_brightness = U8_MAX;
		led->cdev.brightness_get = as3668_brightness_get;
		led->cdev.brightness_set = as3668_brightness_set;

		init_data.fwnode = led->fwnode;
		init_data.default_label = ":";

		err = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
		if (err) {
			dev_err(dev, "failed to register %d LED\n", reg);
			return err;
		}
	}

	return 0;
}

static int as3668_probe(struct i2c_client *client)
{
	u8 chip_id1, chip_id2, chip_serial, chip_rev;
	struct as3668 *as3668;

	/* Check for sensible i2c address */
	if (client->addr != 0x42)
		return dev_err_probe(&client->dev, -EFAULT,
				     "unexpected address for as3668 device\n");

	/* Read identifier from chip */
	chip_id1 = as3668_read_value(client, AS3668_CHIP_ID1);

	if (chip_id1 != AS3668_CHIP_IDENT)
		return dev_err_probe(&client->dev, -ENODEV,
				"chip reported wrong id: 0x%02x\n", chip_id1);

	/* Check the revision */
	chip_id2 = as3668_read_value(client, AS3668_CHIP_ID2);
	chip_serial = FIELD_GET(AS3668_CHIP_ID2_SERIAL_MASK, chip_id2);
	chip_rev = FIELD_GET(AS3668_CHIP_ID2_REV_MASK, chip_id2);

	if (chip_rev != AS3668_CHIP_REV1)
		dev_warn(&client->dev, "unexpected chip revision\n");

	/* Print out information about the chip */
	dev_dbg(&client->dev,
		"chip_id: 0x%02x | chip_id2: 0x%02x | chip_serial: 0x%02x | chip_rev: 0x%02x\n",
		chip_id1, chip_id2, chip_serial, chip_rev);

	as3668 = devm_kzalloc(&client->dev, sizeof(*as3668), GFP_KERNEL);

	if (!as3668)
		return -ENOMEM;

	as3668->client = client;
	int err = as3668_dt_init(as3668);

	if (err) {
		dev_err(&client->dev, "failed to initialize device, err %d\n", err);
		return err;
	}

	/* Initialize the chip */
	as3668_write_value(client, AS3668_CURRX_CONTROL, 0x55);
	as3668_write_value(client, AS3668_CURR1, 0x00);
	as3668_write_value(client, AS3668_CURR2, 0x00);
	as3668_write_value(client, AS3668_CURR3, 0x00);
	as3668_write_value(client, AS3668_CURR4, 0x00);

	return 0;
}

static void as3668_remove(struct i2c_client *client)
{
	as3668_write_value(client, AS3668_CURRX_CONTROL, 0x0);
}

static const struct i2c_device_id as3668_idtable[] = {
	{"as3668"},
	{}
};

MODULE_DEVICE_TABLE(i2c, as3668_idtable);

static const struct of_device_id as3668_match_table[] = {
	{.compatible = "ams,as3668"},
	{}
};

MODULE_DEVICE_TABLE(of, as3668_match_table);

static struct i2c_driver as3668_driver = {
	.driver = {
		.name           = "leds_as3668",
		.of_match_table = as3668_match_table,
	},
	.probe          = as3668_probe,
	.remove         = as3668_remove,
	.id_table       = as3668_idtable,
};

module_i2c_driver(as3668_driver);

MODULE_AUTHOR("Lukas Timmermann <linux@timmermann.space>");
MODULE_DESCRIPTION("AS3668 LED driver");
MODULE_LICENSE("GPL");
