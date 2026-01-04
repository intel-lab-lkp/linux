// SPDX-License-Identifier: GPL-2.0+
/**
 * Hardware monitoring driver for Infineon Integrated-pol-voltage-regulators
 * Driver for TDA38725A and TDA38740A
 *
 * Copyright (c) 2025 Infineon Technologies
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regulator/driver.h>
#include "pmbus.h"

#define TDA38725A_IC_DEVICE_ID "\xA9"
#define TDA38740A_IC_DEVICE_ID "\xA8"

static const struct i2c_device_id tda38740a_id[];

enum chips { tda38725a, tda38740a };

struct tda38740a_data {
	enum chips id;
	struct pmbus_driver_info info;
	u32 vout_multiplier[2];
};

#define to_tda38740a_data(x) container_of(x, struct tda38740a_data, info)

static const struct regulator_desc __maybe_unused tda38740a_reg_desc[] = {
	PMBUS_REGULATOR("vout", 0),
};

static int tda38740a_read_word_data(struct i2c_client *client, int page,
				    int phase, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	const struct tda38740a_data *data = to_tda38740a_data(info);
	int ret;

	/* Virtual PMBUS Command not supported */
	if (reg >= PMBUS_VIRT_BASE)
		return -ENXIO;

	switch (reg) {
	case PMBUS_READ_VOUT:
		ret = pmbus_read_word_data(client, page, phase, reg);
		if (ret < 0)
			return ret;
		ret = ((ret * data->vout_multiplier[0]) /
		       data->vout_multiplier[1]);
		break;
	default:
		ret = pmbus_read_word_data(client, page, phase, reg);
		break;
	}

	return ret;
}

static struct pmbus_driver_info tda38740a_info[] = {
	[tda38740a] = {
		.pages = 1,
		.read_word_data = tda38740a_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_POWER] = linear,
		.format[PSC_TEMPERATURE] = linear,

		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
			| PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP
			| PMBUS_HAVE_IIN
			| PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
			| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
			| PMBUS_HAVE_POUT | PMBUS_HAVE_PIN,
#if IS_ENABLED(CONFIG_SENSORS_TDA38740A_REGULATOR)
		.num_regulators = 1,
		.reg_desc = tda38740a_reg_desc,
#endif
	},
};

static int tda38740a_get_device_id(struct i2c_client *client)
{
	u8 device_id[I2C_SMBUS_BLOCK_MAX + 1];
	enum chips id;
	int status;

	status = i2c_smbus_read_block_data(client, PMBUS_IC_DEVICE_ID,
					   device_id);
	if (status < 0 || status > 1) {
		dev_err(&client->dev, "Failed to read Device Id %x\n", status);
		return -ENODEV;
	}

	if (!memcmp(TDA38725A_IC_DEVICE_ID, device_id, strlen(device_id))) {
		id = tda38725a;
	} else if (!memcmp(TDA38740A_IC_DEVICE_ID, device_id,
			   strlen(device_id))) {
		id = tda38740a;
	} else {
		dev_err(&client->dev, "Unsupported device\n");
		return -ENODEV;
	}

	return id;
}

static int tda38740a_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tda38740a_data *data;
	int chip_id;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE |
					     I2C_FUNC_SMBUS_BYTE_DATA |
					     I2C_FUNC_SMBUS_WORD_DATA |
					     I2C_FUNC_SMBUS_BLOCK_DATA))
		return -ENODEV;

	chip_id = tda38740a_get_device_id(client);
	if (chip_id < 0)
		return chip_id;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->id = chip_id;
	memcpy(&data->info, &tda38740a_info[chip_id], sizeof(data->info));

	if (!of_property_read_u32_array(client->dev.of_node, "vout_multiplier",
					data->vout_multiplier,
					ARRAY_SIZE(data->vout_multiplier))) {
		dev_info(&client->dev,
			 "vout_multiplier from Device Tree:%d %d\n",
			 data->vout_multiplier[0], data->vout_multiplier[1]);
	} else {
		dev_info(&client->dev,
			 "vout_multiplier not available from Device Tree");
		data->vout_multiplier[0] = 0x01;
		data->vout_multiplier[1] = 0x01;
		dev_info(&client->dev, "vout_multiplier default value:%d %d\n",
			 data->vout_multiplier[0], data->vout_multiplier[1]);
	}

	return pmbus_do_probe(client, &data->info);
}

static const struct i2c_device_id tda38740a_id[] = { { "tda38725a", tda38725a },
						     { "tda38740a", tda38740a },
						     {} };

MODULE_DEVICE_TABLE(i2c, tda38740a_id);

static const struct of_device_id __maybe_unused tda38740a_of_match[] = {
	{ .compatible = "infineon,tda38725a", .data = (void *)tda38725a },
	{ .compatible = "infineon,tda38740a", .data = (void *)tda38740a },
	{}
};

MODULE_DEVICE_TABLE(of, tda38740a_of_match);

static struct i2c_driver tda38740a_driver = {
	.driver = {
		.name = "tda38740a",
		.of_match_table = of_match_ptr(tda38740a_of_match),
	},
	.probe = tda38740a_probe,
	.id_table = tda38740a_id,
};

module_i2c_driver(tda38740a_driver);

MODULE_AUTHOR("Ashish Yadav <Ashish.Yadav@infineon.com>");
MODULE_DESCRIPTION("PMBus driver for Infineon IPOL");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
