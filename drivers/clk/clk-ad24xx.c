// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD24xx clock driver
 *
 * Copyright (c) 2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/a2b/a2b.h>
#include <linux/a2b/ad24xx.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define AD24XX_NUM_CLKS 2

/* Define some safe macros to make the code more readable */
#define A2B_CLKCFG(_idx)        (!(_idx) ? A2B_CLK1CFG : A2B_CLK2CFG)

#define A2B_CLKCFG_DIV_SHIFT    A2B_CLK1CFG_CLK1DIV_SHIFT
#define A2B_CLKCFG_PDIV_SHIFT   A2B_CLK1CFG_CLK1PDIV_SHIFT

#define A2B_CLKCFG_DIV_MASK     A2B_CLK1CFG_CLK1DIV_MASK
#define A2B_CLKCFG_PDIV_MASK    A2B_CLK1CFG_CLK1PDIV_MASK
#define A2B_CLKCFG_INV_MASK     A2B_CLK1CFG_CLK1INV_MASK
#define A2B_CLKCFG_EN_MASK      A2B_CLK1CFG_CLK1EN_MASK

static_assert(A2B_CLK1CFG_CLK1DIV_MASK  == A2B_CLK2CFG_CLK2DIV_MASK);
static_assert(A2B_CLK1CFG_CLK1PDIV_MASK == A2B_CLK2CFG_CLK2PDIV_MASK);
static_assert(A2B_CLK1CFG_CLK1INV_MASK  == A2B_CLK2CFG_CLK2INV_MASK);
static_assert(A2B_CLK1CFG_CLK1EN_MASK   == A2B_CLK2CFG_CLK2EN_MASK);

struct ad24xx_clkout {
	struct clk_hw hw;
	unsigned int idx;
	bool registered;
};

struct ad24xx_clk {
	struct device *dev;
	struct a2b_func *func;
	struct a2b_node *node;
	struct regmap *regmap;
	struct clk_hw *pll_hw;
	struct ad24xx_clkout clkouts[AD24XX_NUM_CLKS];
};

static struct ad24xx_clkout *to_ad24xx_clkout(struct clk_hw *hw)
{
	return container_of(hw, struct ad24xx_clkout, hw);
}

static struct ad24xx_clk *to_ad24xx_clk(struct ad24xx_clkout *clkout)
{
	return container_of(clkout, struct ad24xx_clk, clkouts[clkout->idx]);
}

/*
 * A CLKOUT signal is derived from the PLL frequency (2048 * SFF), going through
 * a pre-divide step and a divide step.
 *
 * The pre-divide is either 2 or 32. The divisor is between 1 and 16.
 *
 * The pre-divide register PDIV is 1 bit and selects between 2 (0) or 32 (1).
 * The divide register DIV is 4 bit and the resultant divisor is 2 * (DIV + 1).
 */

#define VAL(_pdiv, _div) \
	(((_pdiv) << A2B_CLKCFG_PDIV_SHIFT) | ((_div) << A2B_CLKCFG_DIV_SHIFT))
#define DIV(_div) (2 * ((_div) + 1))

/* In total there are 6 bits to the value, with the 4th bit going unused */
#define AD24XX_CLK_DIV_WIDTH 6
static const struct clk_div_table ad24xx_clk_div_table[] = {
	{ VAL(0, 0), 2 * DIV(0) },    { VAL(0, 1), 2 * DIV(1) },
	{ VAL(0, 2), 2 * DIV(2) },    { VAL(0, 3), 2 * DIV(3) },
	{ VAL(0, 4), 2 * DIV(4) },    { VAL(0, 5), 2 * DIV(5) },
	{ VAL(0, 6), 2 * DIV(6) },    { VAL(0, 7), 2 * DIV(7) },
	{ VAL(0, 8), 2 * DIV(8) },    { VAL(0, 9), 2 * DIV(9) },
	{ VAL(0, 10), 2 * DIV(10) },  { VAL(0, 11), 2 * DIV(11) },
	{ VAL(0, 12), 2 * DIV(12) },  { VAL(0, 13), 2 * DIV(13) },
	{ VAL(0, 14), 2 * DIV(14) },  { VAL(0, 15), 2 * DIV(15) },
	{ VAL(1, 0), 32 * DIV(0) },   { VAL(1, 1), 32 * DIV(1) },
	{ VAL(1, 2), 32 * DIV(2) },   { VAL(1, 3), 32 * DIV(3) },
	{ VAL(1, 4), 32 * DIV(4) },   { VAL(1, 5), 32 * DIV(5) },
	{ VAL(1, 6), 32 * DIV(6) },   { VAL(1, 7), 32 * DIV(7) },
	{ VAL(1, 8), 32 * DIV(8) },   { VAL(1, 9), 32 * DIV(9) },
	{ VAL(1, 10), 32 * DIV(10) }, { VAL(1, 11), 32 * DIV(11) },
	{ VAL(1, 12), 32 * DIV(12) }, { VAL(1, 13), 32 * DIV(13) },
	{ VAL(1, 14), 32 * DIV(14) }, { VAL(1, 15), 32 * DIV(15) },
	{ /* sentinel */ }
};

static int ad24xx_clk_prepare(struct clk_hw *hw)
{
	struct ad24xx_clkout *clkout = to_ad24xx_clkout(hw);
	struct ad24xx_clk *adclk = to_ad24xx_clk(clkout);
	unsigned int idx = clkout->idx;

	return regmap_update_bits(adclk->regmap, A2B_CLKCFG(idx),
				  A2B_CLKCFG_EN_MASK,
				  FIELD_PREP(A2B_CLKCFG_EN_MASK, 1));
}

static void ad24xx_clk_unprepare(struct clk_hw *hw)
{
	struct ad24xx_clkout *clkout = to_ad24xx_clkout(hw);
	struct ad24xx_clk *adclk = to_ad24xx_clk(clkout);
	unsigned int idx = clkout->idx;

	regmap_update_bits(adclk->regmap, A2B_CLKCFG(idx), A2B_CLKCFG_EN_MASK,
			   FIELD_PREP(A2B_CLKCFG_EN_MASK, 0));
}

static unsigned long ad24xx_clk_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct ad24xx_clkout *clkout = to_ad24xx_clkout(hw);
	struct ad24xx_clk *adclk = to_ad24xx_clk(clkout);
	unsigned int idx = clkout->idx;
	unsigned int val;
	int ret;

	ret = regmap_read(adclk->regmap, A2B_CLKCFG(idx), &val);
	if (ret)
		return 0;

	val &= A2B_CLKCFG_PDIV_MASK | A2B_CLKCFG_DIV_MASK;

	return divider_recalc_rate(hw, parent_rate, val, ad24xx_clk_div_table,
				   0, AD24XX_CLK_DIV_WIDTH);
}

static long ad24xx_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *parent_rate)
{
	return divider_round_rate(hw, rate, parent_rate, ad24xx_clk_div_table,
				  AD24XX_CLK_DIV_WIDTH, 0);
}

static int ad24xx_clk_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	return divider_determine_rate(hw, req, ad24xx_clk_div_table,
				      AD24XX_CLK_DIV_WIDTH, 0);
}

static int ad24xx_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct ad24xx_clkout *clkout = to_ad24xx_clkout(hw);
	struct ad24xx_clk *adclk = to_ad24xx_clk(clkout);
	unsigned int idx = clkout->idx;
	int val;

	val = divider_get_val(rate, parent_rate, ad24xx_clk_div_table,
			      AD24XX_CLK_DIV_WIDTH, 0);
	if (val < 0)
		return val;

	return regmap_update_bits(adclk->regmap, A2B_CLKCFG(idx),
				  A2B_CLKCFG_PDIV_MASK | A2B_CLKCFG_DIV_MASK,
				  val);
}

static int ad24xx_clk_get_phase(struct clk_hw *hw)
{
	struct ad24xx_clkout *clkout = to_ad24xx_clkout(hw);
	struct ad24xx_clk *adclk = to_ad24xx_clk(clkout);
	unsigned int idx = clkout->idx;
	unsigned int val;
	bool invert;
	int ret;

	ret = regmap_read(adclk->regmap, A2B_CLKCFG(idx), &val);
	if (ret)
		return ret;

	invert = FIELD_GET(A2B_CLKCFG_INV_MASK, val);

	return invert ? 180 : 0;
}

static int ad24xx_clk_set_phase(struct clk_hw *hw, int degrees)
{
	struct ad24xx_clkout *clkout = to_ad24xx_clkout(hw);
	struct ad24xx_clk *adclk = to_ad24xx_clk(clkout);
	unsigned int idx = clkout->idx;
	bool invert = !!degrees;

	if (degrees != 0 && degrees != 180)
		return -EINVAL;

	return regmap_update_bits(adclk->regmap, A2B_CLKCFG(idx),
				  A2B_CLKCFG_INV_MASK,
				  FIELD_PREP(A2B_CLKCFG_INV_MASK, invert));
}

static const struct clk_ops ad24xx_clk_ops = {
	.prepare = ad24xx_clk_prepare,
	.unprepare = ad24xx_clk_unprepare,
	.recalc_rate = ad24xx_clk_recalc_rate,
	.round_rate = ad24xx_clk_round_rate,
	.determine_rate = ad24xx_clk_determine_rate,
	.set_rate = ad24xx_clk_set_rate,
	.get_phase = ad24xx_clk_get_phase,
	.set_phase = ad24xx_clk_set_phase,
};

static const struct regmap_config ad24xx_clk_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
};

static struct clk_hw *ad24xx_clk_of_get(struct of_phandle_args *clkspec, void *data)
{
	struct ad24xx_clk *adclk = data;
	unsigned int idx = clkspec->args[0];

	if (idx >= AD24XX_NUM_CLKS)
		return ERR_PTR(-EINVAL);

	if (!adclk->clkouts[idx].registered)
		return ERR_PTR(-ENOENT);

	return &adclk->clkouts[idx].hw;
}

static int ad24xx_clk_probe(struct device *dev)
{
	struct a2b_func *func = to_a2b_func(dev);
	struct a2b_node *node = func->node;
	struct device_node *np = dev->of_node;
	char *pll_name;
	const char *sync_clk_name;
	struct ad24xx_clk *adclk;
	int num_clks;
	int ret;
	int i;

	/*
	 * Older series AD240x and AD241x chips have a single discrete
	 * A2B_CLKCFG register that behaves differently to the A2B_CLKnCFG
	 * registers of the later AD242x series. This driver only supports the
	 * latter right now.
	 */
	if (!(node->chip_info->caps & A2B_CHIP_CAP_CLKOUT))
		return -ENODEV;

	adclk = devm_kzalloc(dev, sizeof(*adclk), GFP_KERNEL);
	if (!adclk)
		return -ENOMEM;

	adclk->regmap =
		devm_regmap_init_a2b_func(func, &ad24xx_clk_regmap_config);
	if (IS_ERR(adclk->regmap))
		return PTR_ERR(adclk->regmap);

	adclk->dev = dev;
	adclk->func = func;
	adclk->node = node;
	dev_set_drvdata(dev, adclk);

	num_clks = of_property_count_strings(np, "clock-output-names");
	if (num_clks < 0 || num_clks > AD24XX_NUM_CLKS)
		return -EINVAL;

	/*
	 * Register the PLL internally to use it as the parent of the CLKOUTs.
	 * The PLL runs at 2048 times the SYNC clock rate.
	 */
	pll_name =
		devm_kasprintf(dev, GFP_KERNEL, "%s_pll", dev_name(&node->dev));
	if (!pll_name)
		return -ENOMEM;
	sync_clk_name = __clk_get_name(a2b_node_get_sync_clk(func->node));
	adclk->pll_hw = devm_clk_hw_register_fixed_factor(
		dev, pll_name, sync_clk_name, 0, 2048, 1);
	if (IS_ERR(adclk->pll_hw))
		return PTR_ERR(adclk->pll_hw);

	for (i = 0; i < num_clks; i++) {
		struct clk_init_data init = { };
		const char *parent_names = clk_hw_get_name(adclk->pll_hw);
		unsigned int idx = i;

		/* Clock outputs can be skipped with the clock-indices property */
		of_property_read_u32_index(np, "clock-indices", i, &idx);
		if (idx > AD24XX_NUM_CLKS)
			return -EINVAL;

		ret = of_property_read_string_index(np, "clock-output-names", i,
						    &init.name);
		if (ret)
			return ret;

		init.ops = &ad24xx_clk_ops;
		init.parent_names = &parent_names;
		init.num_parents = 1;

		adclk->clkouts[idx].hw.init = &init;
		adclk->clkouts[idx].idx = idx;
		adclk->clkouts[idx].registered = true;

		ret = devm_clk_hw_register(dev, &adclk->clkouts[idx].hw);
		if (ret)
			return ret;
	}

	ret = devm_of_clk_add_hw_provider(dev, ad24xx_clk_of_get, adclk);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id ad24xx_clk_of_match_table[] = {
	{ .compatible = "adi,ad2420-clk" },
	{ .compatible = "adi,ad2421-clk" },
	{ .compatible = "adi,ad2422-clk" },
	{ .compatible = "adi,ad2425-clk" },
	{ .compatible = "adi,ad2426-clk" },
	{ .compatible = "adi,ad2427-clk" },
	{ .compatible = "adi,ad2428-clk" },
	{ .compatible = "adi,ad2429-clk" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ad24xx_clk_of_match_table);

static struct a2b_driver ad24xx_clk_driver = {
	.driver = {
		.name = "ad24xx-clk",
		.of_match_table = ad24xx_clk_of_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ad24xx_clk_probe,
};
module_a2b_driver(ad24xx_clk_driver);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("AD24xx CLK driver");
MODULE_LICENSE("GPL");
