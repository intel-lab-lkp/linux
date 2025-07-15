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

static const struct mtk_gate_regs vde20_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x4,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs vde21_cg_regs = {
	.set_ofs = 0x190,
	.clr_ofs = 0x190,
	.sta_ofs = 0x190,
};

static const struct mtk_gate_regs vde22_cg_regs = {
	.set_ofs = 0x200,
	.clr_ofs = 0x204,
	.sta_ofs = 0x200,
};

static const struct mtk_gate_regs vde23_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0xC,
	.sta_ofs = 0x8,
};

#define GATE_VDE20(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde20_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE21(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde21_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_no_setclr_inv,	\
	}

#define GATE_VDE22(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde22_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE23(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde23_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

static const struct mtk_gate vde2_clks[] = {
	/* VDE20 */
	GATE_VDE20(CLK_VDE2_VDEC_CKEN, "vde2_vdec_cken", "vdec_ck", 0),
	GATE_VDE20(CLK_VDE2_VDEC_ACTIVE, "vde2_vdec_active", "vdec_ck", 4),
	GATE_VDE20(CLK_VDE2_VDEC_CKEN_ENG, "vde2_vdec_cken_eng", "vdec_ck", 8),
	/* VDE21 */
	GATE_VDE21(CLK_VDE2_MINI_MDP_CKEN_CFG_RG, "vde2_mini_mdp", "vdec_ck", 0),
	/* VDE22 */
	GATE_VDE22(CLK_VDE2_LAT_CKEN, "vde2_lat_cken", "vdec_ck", 0),
	GATE_VDE22(CLK_VDE2_LAT_ACTIVE, "vde2_lat_active", "vdec_ck", 4),
	GATE_VDE22(CLK_VDE2_LAT_CKEN_ENG, "vde2_lat_cken_eng", "vdec_ck", 8),
	/* VDE23 */
	GATE_VDE23(CLK_VDE2_LARB1_CKEN, "vde2_larb1_cken", "vdec_ck", 0),
};

static const struct mtk_clk_desc vde2_desc = {
	.clks = vde2_clks,
	.num_clks = ARRAY_SIZE(vde2_clks),
};

static const struct of_device_id of_match_clk_mt6789_vdec[] = {
	{
		.compatible = "mediatek,mt6789-vdecsys",
		.data = &vde2_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6789_vdec);

static struct platform_driver clk_mt6789_vdec_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6789-vdec",
		.of_match_table = of_match_clk_mt6789_vdec,
	},
};
module_platform_driver(clk_mt6789_vdec_drv);

MODULE_DESCRIPTION("MediaTek MT6789 Video Decoders clocks driver");
MODULE_LICENSE("GPL");

