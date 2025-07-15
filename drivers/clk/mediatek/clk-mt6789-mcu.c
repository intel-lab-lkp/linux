// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Arseniy Velikanov <me@adomerle.pw>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mediatek,mt6789-clk.h>

#include "clk-mtk.h"

static const char * const mcu_armpll_ll_parents[] = {
	"clk26m",
	"armpll_ll",
	"mainpll",
	"univpll_d2"
};

static const char * const mcu_armpll_bl_parents[] = {
	"clk26m",
	"armpll_bl",
	"mainpll",
	"univpll_d2"
};

static const char * const mcu_armpll_bus_parents[] = {
	"clk26m",
	"ccipll",
	"mainpll",
	"univpll_d2"
};

/*
 * We only configure the CPU muxes when adjust CPU frequency in MediaTek CPUFreq Driver.
 * Other fields like divider always keep the same value. (set once in bootloader)
 */
static struct mtk_composite mcu_muxes[] = {
	/* MP0_PLL_DIV */
	MUX(CLK_MCU_ARMPLL_LL_SEL, "mcu_armpll_ll_sel", mcu_armpll_ll_parents, 0x2A0, 9, 2),
	/* MP1_PLL_DIV */
	MUX(CLK_MCU_ARMPLL_BL_SEL, "mcu_armpll_bl_sel", mcu_armpll_bl_parents, 0x2A4, 9, 2),
	/* BUS_PLL_DIV */
	MUX(CLK_MCU_ARMPLL_BUS_SEL, "mcu_armpll_bus_sel", mcu_armpll_bus_parents, 0x2E0, 9, 2),
};

static const struct mtk_clk_desc mcu_desc = {
	.composite_clks = mcu_muxes,
	.num_composite_clks = ARRAY_SIZE(mcu_muxes),
};

static const struct of_device_id of_match_clk_mt6789_mcu[] = {
	{ .compatible = "mediatek,mt6789-mcusys", .data = &mcu_desc },
	{ /* sentinel */}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6789_mcu);

static struct platform_driver clk_mt6789_mcu_drv = {
	.driver = {
		.name = "clk-mt6789-mcu",
		.of_match_table = of_match_clk_mt6789_mcu,
	},
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
};
module_platform_driver(clk_mt6789_mcu_drv);

MODULE_DESCRIPTION("MediaTek MT6789 mcusys clocks driver");
MODULE_LICENSE("GPL");
