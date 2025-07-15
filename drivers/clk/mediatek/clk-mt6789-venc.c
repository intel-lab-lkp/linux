// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Copyright (c) 2025 Arseniy Velikanov <me@adomerle.pw>
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-mtk.h"
#include "clk-gate.h"

#include <dt-bindings/clock/mediatek,mt6789-clk.h>

static const struct mtk_gate_regs ven1_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_VEN1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven1_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

static const struct mtk_gate ven1_clks[] = {
	GATE_VEN1(CLK_VEN1_CKE0_LARB, "ven1_cke0_larb", "venc_ck", 0),
	GATE_VEN1(CLK_VEN1_CKE1_VENC, "ven1_cke1_venc", "venc_ck", 4),
	GATE_VEN1(CLK_VEN1_CKE2_JPGENC, "ven1_cke2_jpgenc", "venc_ck", 8),
	GATE_VEN1(CLK_VEN1_CKE5_GALS, "ven1_cke5_gals", "venc_ck", 28),
};

static const struct mtk_clk_desc ven1_desc = {
	.clks = ven1_clks,
	.num_clks = ARRAY_SIZE(ven1_clks),
};

static const struct of_device_id of_match_clk_mt6789_venc[] = {
	{
		.compatible = "mediatek,mt6789-vencsys",
		.data = &ven1_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6789_venc);

static struct platform_driver clk_mt6789_venc_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6789-venc",
		.of_match_table = of_match_clk_mt6789_venc,
	},
};
module_platform_driver(clk_mt6789_venc_drv);

MODULE_DESCRIPTION("MediaTek MT6789 Video Encoders clocks driver");
MODULE_LICENSE("GPL");
