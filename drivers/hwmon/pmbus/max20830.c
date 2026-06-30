// SPDX-License-Identifier: GPL-2.0
/*
 * Hardware monitoring driver for Analog Devices MAX20830
 *
 * Copyright (C) 2026 Analog Devices, Inc.
 */

#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/string.h>
#include "pmbus.h"

struct max20830_chip_info {
	const char *id_str;
	u8 id_length;
};

static const struct max20830_chip_info max20830_chip = {
	/*
	 * MAX20830 IC_DEVICE_ID has a byte length of 9 despite being an 8
	 * character string, as it includes a null terminator. The other
	 * devices do not include null.
	 */
	.id_str = "MAX20830\0",
	.id_length = 9,
};

static const struct max20830_chip_info max20830c_chip = {
	.id_str = "MAX20830C",
	.id_length = 9,
};

static const struct max20830_chip_info max20840c_chip = {
	.id_str = "MAX20840C",
	.id_length = 9,
};

struct max20830_data {
	struct pmbus_driver_info info;
	u32 vout_rfb1;
	u32 vout_rfb2;
};

static int max20830_read_word_data(struct i2c_client *client, int page,
				   int phase, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	const struct max20830_data *data = container_of(info, struct max20830_data, info);
	int ret;

	switch (reg) {
	case PMBUS_READ_VOUT:
		ret = pmbus_read_word_data(client, page, phase, reg);
		if (ret < 0)
			return ret;

		/* Apply voltage divider scaling if resistors are non-zero */
		if (data->vout_rfb1 && data->vout_rfb2)
			ret = DIV_ROUND_CLOSEST_ULL((u64)ret * (data->vout_rfb1 +
						    data->vout_rfb2), data->vout_rfb2);
		return ret;
	default:
		return -ENODATA;
	}
}

static struct pmbus_driver_info max20830_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_TEMPERATURE] = linear,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_IOUT |
		PMBUS_HAVE_TEMP |
		PMBUS_HAVE_STATUS_VOUT | PMBUS_HAVE_STATUS_IOUT |
		PMBUS_HAVE_STATUS_INPUT | PMBUS_HAVE_STATUS_TEMP,
	.read_word_data = max20830_read_word_data,
};

static int max20830_probe(struct i2c_client *client)
{
	const struct max20830_chip_info *chip;
	u8 buf[I2C_SMBUS_BLOCK_MAX + 1] = {};
	struct max20830_data *data;
	struct gpio_desc *enable_gpio;
	int ret;

	chip = i2c_get_match_data(client);

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->info = max20830_info;

	/* Read optional voltage divider resistor values */
	device_property_read_u32(&client->dev, "adi,vout-rfb1-ohms", &data->vout_rfb1);
	device_property_read_u32(&client->dev, "adi,vout-rfb2-ohms", &data->vout_rfb2);

	enable_gpio = devm_gpiod_get_optional(&client->dev, "enable", GPIOD_OUT_HIGH);
	if (IS_ERR(enable_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(enable_gpio),
				     "Failed to get enable GPIO\n");

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_READ_BLOCK_DATA) &&
	    !i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return -ENODEV;

	/*
	 * Use i2c_smbus_read_block_data() if supported, otherwise fall back
	 * to i2c_smbus_read_i2c_block_data() to support I2C controllers
	 * which do not support SMBus block reads.
	 */
	if (i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_READ_BLOCK_DATA)) {
		ret = i2c_smbus_read_block_data(client, PMBUS_IC_DEVICE_ID, buf);
		if (ret < 0)
			return dev_err_probe(&client->dev, ret,
					     "Failed to read IC_DEVICE_ID\n");
	} else {
		/* Reads 1 length byte + data bytes */
		ret = i2c_smbus_read_i2c_block_data(client, PMBUS_IC_DEVICE_ID,
						    chip->id_length + 1, buf);
		if (ret < 0)
			return dev_err_probe(&client->dev, ret,
					     "Failed to read IC_DEVICE_ID\n");
		/*
		 * Moves data forward, removing the length byte, this is to
		 * match the format of i2c_smbus_read_block_data().
		 * Also adjust return value to reflect length byte removal.
		 */
		memmove(buf, buf + 1, chip->id_length);
		ret = ret - 1;
	}

	/* Verify we read the expected number of bytes */
	if (ret < chip->id_length)
		return dev_err_probe(&client->dev, -ENODEV,
				     "IC_DEVICE_ID too short: expected %d bytes, got %d\n",
				     chip->id_length, ret);

	/* Null-terminate the string */
	buf[chip->id_length] = '\0';

	/* Verify the device ID matches what we expect */
	if (strncmp(buf, chip->id_str, chip->id_length))
		return dev_err_probe(&client->dev, -ENODEV,
				     "Device mismatch: expected '%s', got '%s'\n",
				     chip->id_str, buf);

	return pmbus_do_probe(client, &data->info);
}

static const struct i2c_device_id max20830_id[] = {
	{ "max20830", (kernel_ulong_t)&max20830_chip },
	{ "max20830c", (kernel_ulong_t)&max20830c_chip },
	{ "max20840c", (kernel_ulong_t)&max20840c_chip },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max20830_id);

static const struct of_device_id max20830_of_match[] = {
	{ .compatible = "adi,max20830", .data = &max20830_chip },
	{ .compatible = "adi,max20830c", .data = &max20830c_chip },
	{ .compatible = "adi,max20840c", .data = &max20840c_chip },
	{ }
};
MODULE_DEVICE_TABLE(of, max20830_of_match);

static struct i2c_driver max20830_driver = {
	.driver = {
		.name = "max20830",
		.of_match_table = max20830_of_match,
	},
	.probe = max20830_probe,
	.id_table = max20830_id,
};

module_i2c_driver(max20830_driver);

MODULE_AUTHOR("Alexis Czezar Torreno <alexisczezar.torreno@analog.com>");
MODULE_DESCRIPTION("PMBus driver for Analog Devices MAX20830");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
