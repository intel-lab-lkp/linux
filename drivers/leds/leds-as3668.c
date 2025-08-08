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
#define AS3668_EXPECTED_I2C_ADDR 0x42

/* Chip Ident */

#define AS3668_CHIP_ID1_REG 0x3e
#define AS3668_CHIP_ID2_REG 0x3f
#define AS3668_CHIP_ID1_EXPECTED_IDENTIFIER 0xa5
#define AS3668_CHIP_ID2_SERIAL_MASK GENMASK(7, 4)
#define AS3668_CHIP_ID2_REV_MASK GENMASK(3, 0)

/* Current Control */

#define AS3668_CURRX_CONTROL_REG 0x01
#define AS3668_CURR1_REG 0x02
#define AS3668_CURR2_REG 0x03
#define AS3668_CURR3_REG 0x04
#define AS3668_CURR4_REG 0x05
#define AS3668_CURRX_MODE_ON 0x1
#define AS3668_CURRX_CURR1_MASK GENMASK(1, 0)
#define AS3668_CURRX_CURR2_MASK GENMASK(3, 2)
#define AS3668_CURRX_CURR3_MASK GENMASK(5, 4)
#define AS3668_CURRX_CURR4_MASK GENMASK(7, 6)

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

static enum led_brightness as3668_brightness_get(struct led_classdev *cdev)
{
	struct as3668_led *led = container_of(cdev, struct as3668_led, cdev);

	return i2c_smbus_read_byte_data(led->chip->client, AS3668_CURR1_REG + led->led_id);
}

static void as3668_brightness_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct as3668_led *led = container_of(cdev, struct as3668_led, cdev);

	int err = i2c_smbus_write_byte_data(led->chip->client,
					    AS3668_CURR1_REG + led->led_id,
					    brightness);

	if (err)
		dev_err(&led->chip->client->dev, "error writing to reg 0x%02x, returned %d\n",
			AS3668_CURR1_REG + led->led_id, err);
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
			return dev_err_probe(dev, err, "'reg' property missing from %s\n",
					     child->name);

		if (reg < 0 || reg > AS3668_MAX_LEDS)
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "'reg' property in %s is out of scope: %d\n",
					     child->name, reg);

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
	u8 chip_ident, chip_subident, chip_serial, chip_rev;

	/* Check for sensible i2c address */
	if (client->addr != AS3668_EXPECTED_I2C_ADDR)
		return dev_err_probe(&client->dev, -EFAULT,
				     "expected i2c address 0x%02x, got 0x%02x\n",
				     AS3668_EXPECTED_I2C_ADDR, client->addr);

	/* Read identifier from chip */
	chip_ident = i2c_smbus_read_byte_data(client, AS3668_CHIP_ID1_REG);

	if (chip_ident != AS3668_CHIP_ID1_EXPECTED_IDENTIFIER)
		return dev_err_probe(&client->dev, -ENODEV,
				     "expected chip identifier 0x%02x, got 0x%02x\n",
				     AS3668_CHIP_ID1_EXPECTED_IDENTIFIER, chip_ident);

	chip_subident = i2c_smbus_read_byte_data(client, AS3668_CHIP_ID2_REG);
	chip_serial = FIELD_GET(AS3668_CHIP_ID2_SERIAL_MASK, chip_subident);
	chip_rev = FIELD_GET(AS3668_CHIP_ID2_REV_MASK, chip_subident);

	/* Print out information about the chip */
	dev_dbg(&client->dev,
		"chip_ident: 0x%02x | chip_subident: 0x%02x | chip_serial: 0x%02x | chip_rev: 0x%02x\n",
		chip_ident, chip_subident, chip_serial, chip_rev);

	as3668 = devm_kzalloc(&client->dev, sizeof(*as3668), GFP_KERNEL);
	if (!as3668)
		return -ENOMEM;

	as3668->client = client;

	err = as3668_dt_init(as3668);
	if (err)
		return err;

	/* Set all four channel modes to 'on' */
	err = i2c_smbus_write_byte_data(client, AS3668_CURRX_CONTROL_REG,
					FIELD_PREP(AS3668_CURRX_CURR1_MASK, AS3668_CURRX_MODE_ON) |
					FIELD_PREP(AS3668_CURRX_CURR2_MASK, AS3668_CURRX_MODE_ON) |
					FIELD_PREP(AS3668_CURRX_CURR3_MASK, AS3668_CURRX_MODE_ON) |
					FIELD_PREP(AS3668_CURRX_CURR4_MASK, AS3668_CURRX_MODE_ON));

	/* Set initial currents to 0mA */
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR1_REG, 0);
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR2_REG, 0);
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR3_REG, 0);
	err |= i2c_smbus_write_byte_data(client, AS3668_CURR4_REG, 0);

	if (err)
		return dev_err_probe(&client->dev, -EIO, "error during hardware initialization\n");

	return 0;
}

static void as3668_remove(struct i2c_client *client)
{
	int err = i2c_smbus_write_byte_data(client, AS3668_CURRX_CONTROL_REG, 0);

	if (err)
		dev_err(&client->dev, "couldn't deinit device\n");
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
