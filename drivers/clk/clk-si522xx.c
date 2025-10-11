// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Skyworks Si522xx PCIe clock generator driver
 *
 * The following series can be supported:
 *   - Si52202 - 2x DIFF
 *   - Si52204 - 4x DIFF
 *   - Si52208 - 8x DIFF
 *   - Si52212 - 12x DIFF
 * Currently tested:
 *   - Si52202
 *
 * Copyright (C) 2025 Marek Vasut <marek.vasut@mailbox.org>
 */

#include <linux/bitfield.h>
#include <linux/bitrev.h>
#include <linux/clk-provider.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

/* Register 0 and 1 (OE1 and OE2) */
#define SI522XX_REG_OE(n)			((n) & 0x1)

/* Register 2 (software spread settings) */
#define SI522XX_REG_SS				0x2
#define SI522XX_REG_SS_SS_EN_SW_HW_CTRL		BIT(7)
#define SI522XX_REG_SS_SS_EN_SW			GENMASK(6, 5)
#define SI522XX_REG_SS_SS_EN_SW_M025P		0
#define SI522XX_REG_SS_SS_EN_SW_OFF		2
#define SI522XX_REG_SS_SS_EN_SW_M050P		3

/* Register 3 (slew rate control) and 4 (slew rate control and amplitude) */
#define SI522XX_REG_SR(n)			(((n) & 0x1) + 3)
#define SI522XX_REG_SR_AMP_MASK			GENMASK(3, 0)
#define SI522XX_REG_SR_AMP_BASE			300000
#define SI522XX_REG_SR_AMP_MIN			600000
#define SI522XX_REG_SR_AMP_DEFAULT		700000
#define SI522XX_REG_SR_AMP_MAX			850000
#define SI522XX_REG_SR_AMP_STEP			50000
#define SI522XX_REG_SR_AMP(UV)			\
	FIELD_PREP(SI522XX_REG_SR_AMP_MASK,	\
		   ((UV) - SI522XX_REG_SR_AMP_BASE) / SI522XX_REG_SR_AMP_STEP)

/* Register 5 and 6 (identification data) */
#define SI522XX_REG_ID				0x5
#define SI522XX_REG_ID_REV			GENMASK(7, 4)
#define SI522XX_REG_ID_VENDOR			GENMASK(3, 0)
#define SI522XX_REG_PG				0x6

/* Count of populated OE bits in control register ref, 0 and 1 */
#define SI522XX_OE_MAP(cr1, cr2)	(((cr2) << 8) | (cr1))
#define SI522XX_OE_MAP_GET_OE(oe, map)	(((map) >> ((oe) * 8)) & 0xff)

#define SI522XX_DIFF_MULT	4
#define SI522XX_DIFF_DIV	1

/* Supported Skyworks Si522xx models. */
enum si522xx_model {
	SI52202 = 0x02,
	SI52204 = 0x04,
	SI52208 = 0x08,
	SI52212 = 0x12,
};

struct si522xx;

struct si_clk {
	struct clk_hw		hw;
	struct si522xx		*si;
	u8			reg;
	u8			bit;
	bool			slew_slow;
};

struct si522xx {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct si_clk		clk_dif[12];
	u16			chip_info;
	u8			pll_amplitude;
	u8			pll_ssc;
};

/*
 * Si522xx i2c regmap
 */
static const struct regmap_range si522xx_readable_ranges[] = {
	regmap_reg_range(SI522XX_REG_OE(0), SI522XX_REG_PG),
};

static const struct regmap_access_table si522xx_readable_table = {
	.yes_ranges = si522xx_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(si522xx_readable_ranges),
};

static const struct regmap_range si522xx_writeable_ranges[] = {
	regmap_reg_range(SI522XX_REG_OE(0), SI522XX_REG_SR(1)),
};

static const struct regmap_access_table si522xx_writeable_table = {
	.yes_ranges = si522xx_writeable_ranges,
	.n_yes_ranges = ARRAY_SIZE(si522xx_writeable_ranges),
};

static const struct regmap_config si522xx_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.reg_base = 0x80,
	.cache_type = REGCACHE_NONE,
	.max_register = SI522XX_REG_PG,
	.rd_table = &si522xx_readable_table,
	.wr_table = &si522xx_writeable_table,
};

static unsigned long si522xx_diff_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	unsigned long long rate;

	rate = (unsigned long long)parent_rate * SI522XX_DIFF_MULT;
	do_div(rate, SI522XX_DIFF_DIV);
	return (unsigned long)rate;
}

static int si522xx_diff_determine_rate(struct clk_hw *hw,
				       struct clk_rate_request *req)
{
	unsigned long best_parent;

	best_parent = (req->rate / SI522XX_DIFF_MULT) * SI522XX_DIFF_DIV;
	req->best_parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw), best_parent);

	req->rate = (req->best_parent_rate / SI522XX_DIFF_DIV) * SI522XX_DIFF_MULT;

	return 0;
}

static int si522xx_diff_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	/*
	 * We must report success but we can do so unconditionally because
	 * si522xx_diff_round_rate returns values that ensure this call is a
	 * nop.
	 */

	return 0;
}

#define to_si522xx_clk(_hw) container_of(_hw, struct si_clk, hw)

static int si522xx_diff_prepare(struct clk_hw *hw)
{
	struct si_clk *si_clk = to_si522xx_clk(hw);
	struct si522xx *si = si_clk->si;

	regmap_update_bits(si->regmap, SI522XX_REG_SR(si_clk->reg), si_clk->bit,
			   si_clk->slew_slow ? 0 : si_clk->bit);
	regmap_set_bits(si->regmap, SI522XX_REG_OE(si_clk->reg), si_clk->bit);

	return 0;
}

static void si522xx_diff_unprepare(struct clk_hw *hw)
{
	struct si_clk *si_clk = to_si522xx_clk(hw);
	struct si522xx *si = si_clk->si;

	regmap_clear_bits(si->regmap, SI522XX_REG_OE(si_clk->reg), si_clk->bit);
}

static const struct clk_ops si522xx_diff_clk_ops = {
	.determine_rate = si522xx_diff_determine_rate,
	.set_rate	= si522xx_diff_set_rate,
	.recalc_rate	= si522xx_diff_recalc_rate,
	.prepare	= si522xx_diff_prepare,
	.unprepare	= si522xx_diff_unprepare,
};

static int si522xx_get_common_config(struct si522xx *si)
{
	struct i2c_client *client = si->client;
	struct device_node *np = client->dev.of_node;
	unsigned int amp, ssc;
	int ret;

	/* Set defaults */
	si->pll_amplitude = SI522XX_REG_SR_AMP(SI522XX_REG_SR_AMP_DEFAULT);
	si->pll_ssc = SI522XX_REG_SS_SS_EN_SW_M050P;

	/* Output clock amplitude */
	ret = of_property_read_u32(np, "skyworks,out-amplitude-microvolt",
				   &amp);
	if (!ret) {
		if (amp < SI522XX_REG_SR_AMP_MIN || amp > SI522XX_REG_SR_AMP_MAX ||
		    amp % SI522XX_REG_SR_AMP_STEP) {
			return dev_err_probe(&client->dev, -EINVAL,
					     "Invalid skyworks,out-amplitude-microvolt value\n");
		}
		si->pll_amplitude = SI522XX_REG_SR_AMP(amp);
	}

	/* Output clock spread spectrum */
	ret = of_property_read_u32(np, "skyworks,out-spread-spectrum", &ssc);
	if (!ret) {
		if (ssc == 100000)	/* 100% ... no spread (default) */
			si->pll_ssc = SI522XX_REG_SS_SS_EN_SW_OFF;
		else if (ssc == 99750)	/* -0.25% ... down spread */
			si->pll_ssc = SI522XX_REG_SS_SS_EN_SW_M025P;
		else if (ssc == 99500)	/* -0.50% ... down spread */
			si->pll_ssc = SI522XX_REG_SS_SS_EN_SW_M050P;
		else
			return dev_err_probe(&client->dev, -EINVAL,
					     "Invalid skyworks,out-spread-spectrum value\n");
	}

	return 0;
}

static int si522xx_get_output_config(struct si522xx *si, int idx)
{
	struct i2c_client *client = si->client;
	unsigned char name[16] = "DIFF0";
	struct device_node *np;
	int ret;
	u32 sr;

	/* Set defaults */
	si->clk_dif[idx].slew_slow = false;

	snprintf(name, sizeof(name), "DIFF%d", idx);
	np = of_get_child_by_name(client->dev.of_node, name);
	if (!np)
		return 0;

	/* Output clock slew rate */
	ret = of_property_read_u32(np, "skyworks,slew-rate", &sr);
	of_node_put(np);
	if (!ret) {
		if (sr == 1900000) {		/* 1.9V/ns */
			si->clk_dif[idx].slew_slow = true;
		} else if (sr == 2400000) {	/* 2.4V/ns (default) */
			si->clk_dif[idx].slew_slow = false;
		} else {
			ret = dev_err_probe(&client->dev, -EINVAL,
					    "Invalid skyworks,slew-rate value\n");
		}
	}

	return ret;
}

static void si522xx_update_config(struct si522xx *si)
{
	/* If amplitude is non-default, update it. */
	if (si->pll_amplitude != SI522XX_REG_SR_AMP(SI522XX_REG_SR_AMP_DEFAULT)) {
		regmap_update_bits(si->regmap, SI522XX_REG_SR(1),
				   SI522XX_REG_SR_AMP_MASK, si->pll_amplitude);
	}

	/* If SSC is non-default, update it. */
	if (si->pll_ssc != SI522XX_REG_SS_SS_EN_SW_M050P) {
		regmap_update_bits(si->regmap, SI522XX_REG_SS,
				   SI522XX_REG_SS_SS_EN_SW_HW_CTRL |
				   SI522XX_REG_SS_SS_EN_SW,
				   SI522XX_REG_SS_SS_EN_SW_HW_CTRL |
				   FIELD_PREP(SI522XX_REG_SS_SS_EN_SW, si->pll_ssc));
	}
}

static void si522xx_diff_idx_to_reg_bit(const u16 chip_info, const int idx,
					struct si_clk *clk)
{
	unsigned long mask;
	int oe, b, ctr = 0;

	for (oe = 0; oe <= 1; oe++) {
		mask = bitrev8(SI522XX_OE_MAP_GET_OE(oe, chip_info));
		for_each_set_bit(b, &mask, 8) {
			if (ctr++ != idx)
				continue;
			clk->reg = SI522XX_REG_OE(oe);
			clk->bit = BIT(7 - b);
			return;
		}
	}
}

static struct clk_hw *
si522xx_of_clk_get(struct of_phandle_args *clkspec, void *data)
{
	struct si522xx *si = data;
	unsigned int idx = clkspec->args[0];

	return &si->clk_dif[idx].hw;
}

static int si522xx_probe(struct i2c_client *client)
{
	const u16 chip_info = (u16)(uintptr_t)i2c_get_match_data(client);
	const struct clk_parent_data clk_parent_data = { .index = 0 };
	struct device *dev = &client->dev;
	unsigned char name[16] = "DIFF0";
	struct clk_init_data init = {};
	struct si522xx *si;
	int i, ret;

	if (!chip_info)
		return -EINVAL;

	si = devm_kzalloc(dev, sizeof(*si), GFP_KERNEL);
	if (!si)
		return -ENOMEM;

	i2c_set_clientdata(client, si);
	si->client = client;

	/* Fetch common configuration from DT (if specified) */
	ret = si522xx_get_common_config(si);
	if (ret)
		return ret;

	/* Fetch DIFFx output configuration from DT (if specified) */
	for (i = 0; i < hweight16(chip_info); i++) {
		ret = si522xx_get_output_config(si, i);
		if (ret)
			return ret;
	}

	/* Get and enable optional power supply regulator */
	ret = devm_regulator_get_enable_optional(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulator\n");

	si->regmap = devm_regmap_init_i2c(client, &si522xx_regmap_config);
	if (IS_ERR(si->regmap))
		return dev_err_probe(dev, PTR_ERR(si->regmap),
				     "Failed to allocate register map\n");

	/* Register clock */
	for (i = 0; i < hweight16(chip_info); i++) {
		memset(&init, 0, sizeof(init));
		snprintf(name, sizeof(name), "DIFF%d", i);
		init.name = name;
		init.ops = &si522xx_diff_clk_ops;
		init.parent_data = &clk_parent_data;
		init.num_parents = 1;
		init.flags = CLK_SET_RATE_PARENT;

		si->clk_dif[i].hw.init = &init;
		si->clk_dif[i].si = si;

		si522xx_diff_idx_to_reg_bit(chip_info, i, &si->clk_dif[i]);

		ret = devm_clk_hw_register(dev, &si->clk_dif[i].hw);
		if (ret)
			return ret;
	}

	/* Wait t_STABLE = 5ms */
	usleep_range(5000, 6000);

	ret = devm_of_clk_add_hw_provider(dev, si522xx_of_clk_get, si);
	if (!ret)
		si522xx_update_config(si);

	return ret;
}

static int __maybe_unused si522xx_suspend(struct device *dev)
{
	struct si522xx *si = dev_get_drvdata(dev);

	regcache_cache_only(si->regmap, true);
	regcache_mark_dirty(si->regmap);

	return 0;
}

static int __maybe_unused si522xx_resume(struct device *dev)
{
	struct si522xx *si = dev_get_drvdata(dev);
	int ret;

	regcache_cache_only(si->regmap, false);
	ret = regcache_sync(si->regmap);
	if (ret)
		dev_err(dev, "Failed to restore register map: %d\n", ret);
	return ret;
}

static const struct i2c_device_id si522xx_id[] = {
	{ "si52202", .driver_data = SI522XX_OE_MAP(0x40, 0x20) },
	{ "si52204", .driver_data = SI522XX_OE_MAP(0x64, 0x10) },
	{ "si52208", .driver_data = SI522XX_OE_MAP(0x67, 0xd0) },
	{ "si52212", .driver_data = SI522XX_OE_MAP(0xff, 0xf0) },
	{ }
};
MODULE_DEVICE_TABLE(i2c, si522xx_id);

static const struct of_device_id clk_si522xx_of_match[] = {
	{ .compatible = "skyworks,si52202", .data = (void *)SI522XX_OE_MAP(0x40, 0x20) },
	{ .compatible = "skyworks,si52204", .data = (void *)SI522XX_OE_MAP(0x64, 0x10) },
	{ .compatible = "skyworks,si52208", .data = (void *)SI522XX_OE_MAP(0x67, 0xd0) },
	{ .compatible = "skyworks,si52212", .data = (void *)SI522XX_OE_MAP(0xff, 0xf0) },
	{ }
};
MODULE_DEVICE_TABLE(of, clk_si522xx_of_match);

static SIMPLE_DEV_PM_OPS(si522xx_pm_ops, si522xx_suspend, si522xx_resume);

static struct i2c_driver si522xx_driver = {
	.driver = {
		.name = "clk-si522xx",
		.pm	= &si522xx_pm_ops,
		.of_match_table = clk_si522xx_of_match,
	},
	.probe		= si522xx_probe,
	.id_table	= si522xx_id,
};
module_i2c_driver(si522xx_driver);

MODULE_AUTHOR("Marek Vasut <marek.vasut@mailbox.org>");
MODULE_DESCRIPTION("Skyworks Si522xx PCIe clock generator driver");
MODULE_LICENSE("GPL");
