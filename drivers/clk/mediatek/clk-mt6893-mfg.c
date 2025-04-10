// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 */

#include <dt-bindings/clock/mediatek,mt6893-clk.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs mfgcfg_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_MFG(_id, _name, _parent, _shift)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &mfgcfg_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_SET_RATE_PARENT)

static const struct mtk_gate mfgcfg_clks[] = {
	GATE_MFG(CLK_MFGCFG_BG3D, "mfgcfg_bg3d", "mfg_pll_sel", 0),
};

static const struct mtk_clk_desc mfgcfg_desc = {
	.clks = mfgcfg_clks,
	.num_clks = ARRAY_SIZE(mfgcfg_clks),
};

static const struct of_device_id of_match_clk_mt6893_mfgcfg[] = {
	{ .compatible = "mediatek,mt6893-mfgcfg", .data = &mfgcfg_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_mfgcfg);

static struct platform_driver clk_mt6893_mfgcfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-mfgcfg",
		.of_match_table = of_match_clk_mt6893_mfgcfg,
	},
};

module_platform_driver(clk_mt6893_mfgcfg_drv);

MODULE_DESCRIPTION("MediaTek MT6893 GPU mfg clocks driver");
MODULE_LICENSE("GPL");
