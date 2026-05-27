// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Versal SysMon I2C driver
 *
 * Copyright (C) 2023 - 2026, Advanced Micro Devices, Inc.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include "versal-sysmon.h"

#define SYSMON_I2C_INSTR_READ	BIT(2)
#define SYSMON_I2C_INSTR_WRITE	BIT(3)

#define SYSMON_I2C_DATA0_MASK	GENMASK(7, 0)
#define SYSMON_I2C_DATA1_MASK	GENMASK(15, 8)
#define SYSMON_I2C_DATA2_MASK	GENMASK(23, 16)
#define SYSMON_I2C_DATA3_MASK	GENMASK(31, 24)

#define SYSMON_I2C_OFS_LOW_MASK		GENMASK(9, 2)
#define SYSMON_I2C_OFS_HIGH_MASK	GENMASK(15, 10)

/* Byte positions within the 8-byte I2C command frame (HW-defined) */
enum sysmon_i2c_payload_idx {
	SYSMON_I2C_DATA0_IDX = 0,
	SYSMON_I2C_DATA1_IDX = 1,
	SYSMON_I2C_DATA2_IDX = 2,
	SYSMON_I2C_DATA3_IDX = 3,
	SYSMON_I2C_OFS_LOW_IDX = 4,
	SYSMON_I2C_OFS_HIGH_IDX = 5,
	SYSMON_I2C_INSTR_IDX = 6,
};

static int sysmon_i2c_reg_read(void *context, unsigned int reg,
			       unsigned int *val)
{
	struct i2c_client *client = context;
	u8 write_buf[8] = { };
	u8 read_buf[4];
	int ret;

	write_buf[SYSMON_I2C_OFS_LOW_IDX] =
		FIELD_GET(SYSMON_I2C_OFS_LOW_MASK, reg);
	write_buf[SYSMON_I2C_OFS_HIGH_IDX] =
		FIELD_GET(SYSMON_I2C_OFS_HIGH_MASK, reg);
	write_buf[SYSMON_I2C_INSTR_IDX] = SYSMON_I2C_INSTR_READ;

	ret = i2c_master_send(client, write_buf, sizeof(write_buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(write_buf))
		return -EIO;

	ret = i2c_master_recv(client, read_buf, sizeof(read_buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(read_buf))
		return -EIO;

	*val = FIELD_PREP(SYSMON_I2C_DATA0_MASK,
			  read_buf[SYSMON_I2C_DATA0_IDX]) |
	       FIELD_PREP(SYSMON_I2C_DATA1_MASK,
			  read_buf[SYSMON_I2C_DATA1_IDX]) |
	       FIELD_PREP(SYSMON_I2C_DATA2_MASK,
			  read_buf[SYSMON_I2C_DATA2_IDX]) |
	       FIELD_PREP(SYSMON_I2C_DATA3_MASK,
			  read_buf[SYSMON_I2C_DATA3_IDX]);

	return 0;
}

static int sysmon_i2c_reg_write(void *context, unsigned int reg,
				unsigned int val)
{
	struct i2c_client *client = context;
	u8 write_buf[8] = { };
	int ret;

	write_buf[SYSMON_I2C_DATA0_IDX] =
		FIELD_GET(SYSMON_I2C_DATA0_MASK, val);
	write_buf[SYSMON_I2C_DATA1_IDX] =
		FIELD_GET(SYSMON_I2C_DATA1_MASK, val);
	write_buf[SYSMON_I2C_DATA2_IDX] =
		FIELD_GET(SYSMON_I2C_DATA2_MASK, val);
	write_buf[SYSMON_I2C_DATA3_IDX] =
		FIELD_GET(SYSMON_I2C_DATA3_MASK, val);
	write_buf[SYSMON_I2C_OFS_LOW_IDX] =
		FIELD_GET(SYSMON_I2C_OFS_LOW_MASK, reg);
	write_buf[SYSMON_I2C_OFS_HIGH_IDX] =
		FIELD_GET(SYSMON_I2C_OFS_HIGH_MASK, reg);
	write_buf[SYSMON_I2C_INSTR_IDX] = SYSMON_I2C_INSTR_WRITE;

	ret = i2c_master_send(client, write_buf, sizeof(write_buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(write_buf))
		return -EIO;

	return 0;
}

static const struct regmap_config sysmon_i2c_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = SYSMON_REG_STRIDE,
	.max_register = SYSMON_MAX_REG,
	.reg_read = sysmon_i2c_reg_read,
	.reg_write = sysmon_i2c_reg_write,
};

static int sysmon_i2c_probe(struct i2c_client *client)
{
	struct regmap *regmap;

	regmap = devm_regmap_init(&client->dev, NULL, client,
				  &sysmon_i2c_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* I2C has no IRQ connection; events are not supported */
	return sysmon_core_probe(&client->dev, regmap);
}

static const struct of_device_id sysmon_i2c_of_match_table[] = {
	{ .compatible = "xlnx,versal-sysmon" },
	{ }
};
MODULE_DEVICE_TABLE(of, sysmon_i2c_of_match_table);

static const struct i2c_device_id sysmon_i2c_id_table[] = {
	{ "versal-sysmon" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sysmon_i2c_id_table);

static struct i2c_driver sysmon_i2c_driver = {
	.probe = sysmon_i2c_probe,
	.driver = {
		.name = "versal-sysmon-i2c",
		.of_match_table = sysmon_i2c_of_match_table,
	},
	.id_table = sysmon_i2c_id_table,
};
module_i2c_driver(sysmon_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD Versal SysMon I2C Driver");
MODULE_AUTHOR("Conall O'Griofa <conall.ogriofa@amd.com>");
MODULE_AUTHOR("Salih Erim <salih.erim@amd.com>");
