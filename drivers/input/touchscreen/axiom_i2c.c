// SPDX-License-Identifier: GPL-2.0
/*
 * TouchNetix aXiom Touchscreen Driver
 *
 * Copyright (C) 2020-2026 TouchNetix Ltd.
 *
 * Author(s): Bart Prescott <bartp@baasheep.co.uk>
 *            Pedro Torruella <pedro.torruella@touchnetix.com>
 *            Mark Satterthwaite <mark.satterthwaite@touchnetix.com>
 *            Hannah Rossiter <hannah.rossiter@touchnetix.com>
 *            Andrew Thomas <andrew.thomas@touchnetix.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

// #define DEBUG // Enable debug messages

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/input.h>
#include "axiom_core.h"

static int axiom_i2c_read_block_data(struct device *dev, u16 addr, u16 length,
				     void *values)
{
	int error;
	struct i2c_client *client = to_i2c_client(dev);
	struct axiom_cmd_header cmd_header = { .target_address = addr,
					       .length = length,
					       .rd_wr = AX_RD_OP };

	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd_header),
			.buf = (u8 *)&cmd_header,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = values,
		},
	};

	error = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (error < 0) {
		dev_err(dev, "I2C transfer error: %d\n", error);
		return error;
	}

	udelay(AXIOM_HOLDOFF_DELAY_US);

	return error != ARRAY_SIZE(msgs) ? -EIO : 0;
}

static int axiom_i2c_write_block_data(struct device *dev, u16 addr, u16 length,
				      void *values)
{
	int error;
	struct i2c_client *client = to_i2c_client(dev);
	struct axiom_cmd_header cmd_header = { .target_address = addr,
					       .length = length,
					       .rd_wr = AX_WR_OP };

	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = sizeof(cmd_header),
			.buf = (u8 *)&cmd_header,
		},
		{
			.addr = client->addr,
			.flags = 0,
			.len = length,
			.buf = values,
		},
	};

	error = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (error < 0) {
		dev_err(dev, "I2C transfer error: %d\n", error);
		return error;
	}

	udelay(AXIOM_HOLDOFF_DELAY_US);

	return error != ARRAY_SIZE(msgs) ? -EIO : 0;
}

static const struct axiom_bus_ops axiom_i2c_bus_ops = {
	.bustype = BUS_I2C,
	.write = axiom_i2c_write_block_data,
	.read = axiom_i2c_read_block_data,
};

static int axiom_i2c_probe(struct i2c_client *client)
{
	struct axiom *axiom;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C functionality not Supported\n");
		return -EIO;
	}

	axiom = axiom_probe(&axiom_i2c_bus_ops, &client->dev, client->irq);
	if (IS_ERR(axiom))
		return dev_err_probe(&client->dev, PTR_ERR(axiom),
				     "failed to register input device\n");

	return 0;
}

static const struct i2c_device_id axiom_i2c_id_table[] = {
	{ "axiom-i2c" },
	{},
};
MODULE_DEVICE_TABLE(i2c, axiom_i2c_id_table);

static const struct of_device_id axiom_i2c_dt_ids[] = {
	{
		.compatible = "tnx,axiom-i2c",
		.data = "axiom",
	},
	{}
};
MODULE_DEVICE_TABLE(of, axiom_i2c_dt_ids);

static struct i2c_driver axiom_i2c_driver = {
	.driver = {
		.name = "axiom_i2c",
		.of_match_table = of_match_ptr(axiom_i2c_dt_ids),
	},
	.id_table = axiom_i2c_id_table,
	.probe = axiom_i2c_probe,
};

module_i2c_driver(axiom_i2c_driver);

MODULE_AUTHOR("TouchNetix <support@touchnetix.com>");
MODULE_DESCRIPTION("aXiom touchscreen I2C bus driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("axiom");
MODULE_VERSION("1.0.0");
