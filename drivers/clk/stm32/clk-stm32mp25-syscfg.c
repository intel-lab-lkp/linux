// SPDX-License-Identifier: GPL-2.0-only
/*
 * STM32MP25 SYSCFG clocks
 *
 * Copyright (C) 2026 Amarula Solutions, Dario Binacchi <dario.binacchi@amarulasolutions.com>
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define SYSCFG_DISPLAYCLKCR		0x5000
#define DISPLAYCLKCR_DPIVSLVDS		GENMASK(1, 0)

/*
 * The LTDC pixel clock source is selected in SYSCFG_DISPLAYCLKCR among
 * the DSI byte lane clock, the LVDS PHY pixel clock (reset value) and
 * the LTDC kernel clock, which is used with the RGB (DPI) output.
 */
static const char * const clk_pix_ltdc_parents[] = {
	"txbyteclk", "clk_pix_lvds", "ck_ker_ltdc",
};

struct stm32mp25_syscfg_mux {
	struct clk_hw hw;
	struct regmap *regmap;
};

static inline struct stm32mp25_syscfg_mux *to_syscfg_mux(struct clk_hw *hw)
{
	return container_of(hw, struct stm32mp25_syscfg_mux, hw);
}

static u8 stm32mp25_syscfg_mux_get_parent(struct clk_hw *hw)
{
	struct stm32mp25_syscfg_mux *mux = to_syscfg_mux(hw);
	unsigned int val;

	regmap_read(mux->regmap, SYSCFG_DISPLAYCLKCR, &val);

	return FIELD_GET(DISPLAYCLKCR_DPIVSLVDS, val);
}

static int stm32mp25_syscfg_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct stm32mp25_syscfg_mux *mux = to_syscfg_mux(hw);

	return regmap_update_bits(mux->regmap, SYSCFG_DISPLAYCLKCR,
				  DISPLAYCLKCR_DPIVSLVDS,
				  FIELD_PREP(DISPLAYCLKCR_DPIVSLVDS, index));
}

static const struct clk_ops stm32mp25_syscfg_mux_ops = {
	.get_parent = stm32mp25_syscfg_mux_get_parent,
	.set_parent = stm32mp25_syscfg_mux_set_parent,
	.determine_rate = clk_hw_determine_rate_no_reparent,
};

static int stm32mp25_syscfg_clk_probe(struct auxiliary_device *adev,
				      const struct auxiliary_device_id *id)
{
	struct device *dev = &adev->dev;
	struct clk_init_data init = { };
	struct stm32mp25_syscfg_mux *mux;
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->regmap = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(mux->regmap))
		return dev_err_probe(dev, PTR_ERR(mux->regmap),
				     "failed to get the SYSCFG regmap\n");

	init.name = "clk_pix_ltdc";
	init.ops = &stm32mp25_syscfg_mux_ops;
	init.parent_names = clk_pix_ltdc_parents;
	init.num_parents = ARRAY_SIZE(clk_pix_ltdc_parents);
	init.flags = CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT;
	mux->hw.init = &init;

	ret = devm_clk_hw_register(dev, &mux->hw);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register %s\n",
				     init.name);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &mux->hw);
}

static const struct auxiliary_device_id stm32mp25_syscfg_clk_ids[] = {
	{ .name = "stm32mp2_syscfg.clk" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, stm32mp25_syscfg_clk_ids);

static struct auxiliary_driver stm32mp25_syscfg_clk_driver = {
	.probe = stm32mp25_syscfg_clk_probe,
	.id_table = stm32mp25_syscfg_clk_ids,
};
module_auxiliary_driver(stm32mp25_syscfg_clk_driver);

MODULE_AUTHOR("Dario Binacchi <dario.binacchi@amarulasolutions.com>");
MODULE_DESCRIPTION("STM32MP25 SYSCFG clock driver");
MODULE_LICENSE("GPL");
