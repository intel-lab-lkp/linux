// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP P3T175x Temperature Sensor Driver
 *
 * Copyright 2025 NXP
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/events.h>

#include "p3t1755.h"

static const struct regmap_config p3t1755_i2c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const struct of_device_id p3t1755_i2c_of_match[] = {
	{ .compatible = "nxp,p3t1755-iio", .data = &p3t1755_channels_info },
	{ .compatible = "nxp,p3t1750-iio", .data = &p3t1750_channels_info },
	{ }
};
MODULE_DEVICE_TABLE(of, p3t1755_i2c_of_match);

static const struct i2c_device_id p3t1755_i2c_id_table[] = {
	{ "p3t1755", (kernel_ulong_t)&p3t1755_channels_info },
	{ "p3t1750", (kernel_ulong_t)&p3t1750_channels_info},
	{ }
};
MODULE_DEVICE_TABLE(i2c, p3t1755_i2c_id_table);

static int p3t1755_i2c_probe(struct i2c_client *client)
{
	const struct p3t1755_info *chip;
	struct regmap *regmap;
	bool tm_mode = false;
	int fq_bits = -1;
	int ret;
	u32 fq;

	regmap = devm_regmap_init_i2c(client, &p3t1755_i2c_regmap_config);
	if (IS_ERR(regmap)) {
		return dev_err_probe(&client->dev, PTR_ERR(regmap),
				     "regmap init failed\n");
	}

	tm_mode = device_property_read_bool(&client->dev, "nxp,interrupt-mode");

	if (!device_property_read_u32(&client->dev, "nxp,fault-queue", &fq)) {
		fq_bits = p3t1755_fault_queue_to_bits(fq);
		if (fq_bits < 0) {
			return dev_err_probe(&client->dev, fq_bits,
						     "invalid nxp,fault-queue %u (1/2/4/6)\n", fq);
			}
	}

	dev_dbg(&client->dev, "Using TM mode: %s\n",
		tm_mode ? "Interrupt" : "Comparator");

	chip = i2c_get_match_data(client);

	dev_dbg(&client->dev, "Registering p3t175x temperature sensor");

	ret = p3t1755_probe(&client->dev, chip, regmap,
			    tm_mode, fq_bits, client->irq);

	if (ret) {
		dev_err_probe(&client->dev, ret, "p3t175x probe failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static struct i2c_driver p3t1755_driver = {
	.driver = {
		.name = "p3t175x_i2c",
		.of_match_table = p3t1755_i2c_of_match,
	},
	.probe = p3t1755_i2c_probe,
	.id_table = p3t1755_i2c_id_table,
};
module_i2c_driver(p3t1755_driver);

MODULE_AUTHOR("Lakshay Piplani <lakshay.piplani@nxp.com>");
MODULE_DESCRIPTION("NXP P3T175x I2C Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(IIO_P3T1755);
