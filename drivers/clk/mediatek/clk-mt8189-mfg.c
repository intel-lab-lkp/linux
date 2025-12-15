// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Qiqi Wang <qiqi.wang@mediatek.com>
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt8189-clk.h>

static const struct mtk_gate_regs mfg_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_MFG(_id, _name, _parent, _shift)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &mfg_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_IGNORE_UNUSED)

static const struct mtk_gate mfg_clks[] = {
	GATE_MFG(CLK_MFG_BG3D, "mfg_bg3d", "mfg_sel_mfgpll", 0),
};

static const struct mtk_clk_desc mfg_mcd = {
	.clks = mfg_clks,
	.num_clks = ARRAY_SIZE(mfg_clks),
};

static const struct of_device_id of_match_clk_mt8189_mfg[] = {
	{ .compatible = "mediatek,mt8189-mfgcfg", .data = &mfg_mcd },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8189_mfg);

static struct platform_driver clk_mt8189_mfg_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8189-mfg",
		.of_match_table = of_match_clk_mt8189_mfg,
	},
};

module_platform_driver(clk_mt8189_mfg_drv);
MODULE_DESCRIPTION("MediaTek MT8189 mfg clocks driver");
MODULE_LICENSE("GPL");
