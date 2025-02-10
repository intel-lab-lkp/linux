#include <linux/clk.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/clk-provider.h>
#include "../../../drivers/clk/meson/clk-regmap.h"
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include <dt-bindings/sound/meson-aiu.h>
#include "aiu.h"

static struct clk_regmap i2s_aoclk_div_gate = {
	.data = &(struct clk_regmap_gate_data){
		.offset = AIU_CLK_CTRL,
		.bit_idx = 0,
		.flags = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "i2s_aoclk_div_gate",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]) {
			"cts_amclk",
		},
		.num_parents = 1,
		.flags = 0,
	},
};

static struct clk_regmap i2s_aoclk_basic_divider = {
	.data = &(struct clk_regmap_div_data){
		.offset = AIU_CLK_CTRL,
		.shift = 2,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "i2s_aoclk_basic_divider",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]) {
			"i2s_aoclk_div_gate",
		},
		.num_parents = 1,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
};

static struct clk_regmap i2s_aoclk_more_divider = {
	.data = &(struct clk_regmap_div_data){
		.offset = AIU_CLK_CTRL_MORE,
		.shift = 0,
		.width = 6,
		.flags = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "i2s_aoclk_more_divider",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]) {
			"i2s_aoclk_basic_divider",
		},
		.num_parents = 1,
		.flags = 0,
	},
};

static struct clk_regmap i2s_lrclk_divider = {
	.data = &(struct clk_regmap_div_data){
		.offset = AIU_CODEC_DAC_LRCLK_CTRL,
		.shift = 0,
		.width = 12,
		.flags = 0,
	},
	.hw.init = &(struct clk_init_data){
		.name = "i2s_lrlk_divider",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]) {
			"i2s_aoclk_more_divider",
		},
		.num_parents = 1,
		.flags = 0,
	},
};

struct clk_regmap *const aiu_clk_regmaps[] = {
	&i2s_aoclk_div_gate,
	&i2s_aoclk_basic_divider,
	&i2s_aoclk_more_divider,
	&i2s_lrclk_divider,
};

static struct clk_hw *aiu_clk_hw_get(struct of_phandle_args *clkspec, void *clk_hw_data)
{
	struct clk_regmap **const aiu_clk_regmaps_ptr = clk_hw_data;
	unsigned int idx = clkspec->args[0];

	if (idx >= ARRAY_SIZE(aiu_clk_regmaps)) {
		pr_err("%s: invalid index %u\n", __func__, idx);
		return ERR_PTR(-EINVAL);
	}

	return &(aiu_clk_regmaps_ptr[idx]->hw);
}

int aiu_register_clocks(struct device *dev, struct regmap *map)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(aiu_clk_regmaps); i++) {
		aiu_clk_regmaps[i]->map = map;
		ret = devm_clk_hw_register(dev, &(aiu_clk_regmaps[i]->hw));
		if (ret) {
			dev_err(dev, "Failed to register AIU clock %d\n", i);
			return ret;
		}
	}

	ret = devm_of_clk_add_hw_provider(dev, aiu_clk_hw_get, (void *)&aiu_clk_regmaps);
	if (ret) {
		dev_err(dev, "devm_of_clk_add_hw_provider failed\n");
		return ret;
	}

	return 0;
}
