// SPDX-License-Identifier: GPL-2.0-only
/*
 * MFD core driver for the X-Powers AC200
 *
 * Copyright (C) 2019 Jernej Skrabec <jernej.skrabec@gmail.com>
 * Copyright (C) 2026 James Hilliard <james.hilliard1@gmail.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define AC200_SYS_CONTROL_REG			0x0002
#define AC200_SYS_CONTROL_CHIP_RESET_DEASSERT	BIT(0)

/* Interface register accessible from every register page. */
#define AC200_TWI_REG_ADDR_H	0x00fe
#define AC200_MAX_REG		0xa1f2

static const struct regmap_range_cfg ac200_range_cfg[] = {
	{
		.range_max = AC200_MAX_REG,
		.selector_reg = AC200_TWI_REG_ADDR_H,
		.selector_mask = 0xff,
		.window_len = 256,
	},
};

/*
 * Each AC200 sub-block can reset independently, invalidating its register
 * contents without regmap's knowledge. Cache only the common page selector;
 * this avoids a selector read-modify-write for every access on the same page
 * without ever returning stale functional-register values.
 */
static bool ac200_volatile_reg(struct device *dev, unsigned int reg)
{
	return reg != AC200_TWI_REG_ADDR_H;
}

static const struct regmap_config ac200_regmap_config = {
	.name = "ac200",
	.reg_bits = 8,
	.reg_stride = 2,
	.val_bits = 16,
	.ranges = ac200_range_cfg,
	.num_ranges = ARRAY_SIZE(ac200_range_cfg),
	.max_register = AC200_MAX_REG,
	.volatile_reg = ac200_volatile_reg,
	.cache_type = REGCACHE_MAPLE,
};

static int ac200_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regmap *regmap;
	struct clk *clk;
	int ret;

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "failed to enable input clock\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to lock clock rate\n");

	regmap = devm_regmap_init_i2c(client, &ac200_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to initialize regmap\n");

	/*
	 * No minimum delay is documented. Match the vendor driver's 40 ms delay
	 * before its first AC200 register access after enabling the input clock.
	 */
	msleep(40);

	ret = regmap_set_bits(regmap, AC200_SYS_CONTROL_REG,
			      AC200_SYS_CONTROL_CHIP_RESET_DEASSERT);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ac200_of_match[] = {
	{ .compatible = "x-powers,ac200" },
	{ }
};
MODULE_DEVICE_TABLE(of, ac200_of_match);

static const struct i2c_device_id ac200_i2c_ids[] = {
	{ .name = "ac200" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ac200_i2c_ids);

static struct i2c_driver ac200_driver = {
	.driver = {
		.name = "ac200",
		.of_match_table = ac200_of_match,
	},
	.probe = ac200_probe,
	.id_table = ac200_i2c_ids,
};
module_i2c_driver(ac200_driver);

MODULE_AUTHOR("James Hilliard <james.hilliard1@gmail.com>");
MODULE_DESCRIPTION("X-Powers AC200 MFD core driver");
MODULE_LICENSE("GPL");
