// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Bharadwaj Raju <bharadwaj.raju777@gmail.com>
 */

 #include "inv_icm20948.h"

static const struct regmap_range_cfg inv_icm20948_regmap_ranges[] = {
	{
		.name = "user banks",
		.range_min = 0x0000,
		.range_max = 0x3FFF,
		.selector_reg = INV_ICM20948_REG_BANK_SEL,
		.selector_mask = INV_ICM20948_BANK_SEL_MASK,
		.window_start = 0,
		.window_len = 0x1000,
	},
};

static const struct regmap_range inv_icm20948_regmap_volatile_yes_ranges[] = {
	/* WHOAMI */
	regmap_reg_range(0x0000, 0x0000),
	/* PWR_MGMT_1 */
	regmap_reg_range(0x0006, 0x0006),
	/* I2C and INT status */
	regmap_reg_range(0x0017, 0x001C),
	/* Sensor readouts */
	regmap_reg_range(0x0028, 0x0052),
	/* FIFO count and data */
	regmap_reg_range(0x0070, 0x0072),
	/* Data ready status */
	regmap_reg_range(0x0074, 0x0074),
	/* GYRO_CONFIG_1 */
	regmap_reg_range(0x2001, 0x2001),
	/* I2C SLV4 data in */
	regmap_reg_range(0x307F, 0x307F),
};

static const struct regmap_access_table inv_icm20948_regmap_volatile_accesses = {
	.yes_ranges = inv_icm20948_regmap_volatile_yes_ranges,
	.n_yes_ranges = ARRAY_SIZE(inv_icm20948_regmap_volatile_yes_ranges),
};

static const struct regmap_range inv_icm20948_rd_noinc_no_ranges[] = {
	regmap_reg_range(0x0000, INV_ICM20948_REG_FIFO_RW - 1),
	regmap_reg_range(INV_ICM20948_REG_FIFO_RW + 1, 0x3FFF),
};

static const struct regmap_access_table inv_icm20948_regmap_rd_noinc_table = {
	.no_ranges = inv_icm20948_rd_noinc_no_ranges,
	.n_no_ranges = ARRAY_SIZE(inv_icm20948_rd_noinc_no_ranges),
};

const struct regmap_config inv_icm20948_regmap_config = {
	.name = "inv_icm20948",
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x3FFF,
	.ranges = inv_icm20948_regmap_ranges,
	.num_ranges = ARRAY_SIZE(inv_icm20948_regmap_ranges),
	.volatile_table = &inv_icm20948_regmap_volatile_accesses,
	.rd_noinc_table = &inv_icm20948_regmap_rd_noinc_table,
	.cache_type = REGCACHE_MAPLE,
};
EXPORT_SYMBOL_NS_GPL(inv_icm20948_regmap_config, "IIO_ICM20948");

static int inv_icm20948_setup(struct inv_icm20948_state *state)
{
	scoped_guard(mutex, &state->lock) {
		int reported_whoami;
		int ret = regmap_read(state->regmap, INV_ICM20948_REG_WHOAMI,
				      &reported_whoami);
		if (ret)
			return ret;
		if (reported_whoami != INV_ICM20948_WHOAMI) {
			dev_err(state->dev, "invalid whoami %d, expected %d\n",
				reported_whoami, INV_ICM20948_WHOAMI);
			return -ENODEV;
		}

		ret = regmap_write_bits(state->regmap, INV_ICM20948_REG_PWR_MGMT_1,
					INV_ICM20948_PWR_MGMT_1_DEV_RESET,
					INV_ICM20948_PWR_MGMT_1_DEV_RESET);
		if (ret)
			return ret;
		msleep(INV_ICM20948_SLEEP_WAKEUP_MS);

		ret = regmap_write_bits(state->regmap, INV_ICM20948_REG_PWR_MGMT_1,
					INV_ICM20948_PWR_MGMT_1_SLEEP, 0);
		if (ret)
			return ret;

		msleep(INV_ICM20948_SLEEP_WAKEUP_MS);
	}

	state->temp_dev = inv_icm20948_temp_init(state);
	if (IS_ERR(state->temp_dev))
		return PTR_ERR(state->temp_dev);

	state->gyro_dev = inv_icm20948_gyro_init(state);
	if (IS_ERR(state->gyro_dev))
		return PTR_ERR(state->gyro_dev);

	return 0;
}

int inv_icm20948_core_probe(struct regmap *regmap)
{
	struct device *dev = regmap_get_device(regmap);

	struct inv_icm20948_state *state;

	state = devm_kzalloc(dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->regmap = regmap;
	state->dev = dev;

	mutex_init(&state->lock);

	return inv_icm20948_setup(state);
}

MODULE_AUTHOR("Bharadwaj Raju <bharadwaj.raju777@gmail.com>");
MODULE_DESCRIPTION("InvenSense ICM-20948 device driver");
MODULE_LICENSE("GPL");
