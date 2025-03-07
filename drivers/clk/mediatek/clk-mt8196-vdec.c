// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

static const struct mtk_gate_regs vde20_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x4,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs vde20_vote_regs = {
	.set_ofs = 0x0088,
	.clr_ofs = 0x008c,
	.sta_ofs = 0x2c44,
};

static const struct mtk_gate_regs vde21_cg_regs = {
	.set_ofs = 0x200,
	.clr_ofs = 0x204,
	.sta_ofs = 0x200,
};

static const struct mtk_gate_regs vde21_vote_regs = {
	.set_ofs = 0x0080,
	.clr_ofs = 0x0084,
	.sta_ofs = 0x2c40,
};

static const struct mtk_gate_regs vde22_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0xc,
	.sta_ofs = 0x8,
};

static const struct mtk_gate_regs vde22_vote_regs = {
	.set_ofs = 0x0078,
	.clr_ofs = 0x007c,
	.sta_ofs = 0x2c3c,
};

#define GATE_VDE20(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde20_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE20_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE20(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &vde20_cg_regs,			\
		.vote_regs = &vde20_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VDE21(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde21_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE21_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE21(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &vde21_cg_regs,			\
		.vote_regs = &vde21_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VDE22(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde22_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE22_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE22(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &vde22_cg_regs,			\
		.vote_regs = &vde22_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
	}

static const struct mtk_gate vde2_clks[] = {
	/* VDE20 */
	GATE_VOTE_VDE20(CLK_VDE2_VDEC_CKEN, "vde2_vdec_cken", "ck2_vdec_ck", 0),
	GATE_VDE20_V(CLK_VDE2_VDEC_CKEN_VDEC, "vde2_vdec_cken_vdec", "vde2_vdec_cken"),
	GATE_VOTE_VDE20(CLK_VDE2_VDEC_ACTIVE, "vde2_vdec_active", "ck2_vdec_ck", 4),
	GATE_VDE20_V(CLK_VDE2_VDEC_ACTIVE_VDEC, "vde2_vdec_active_vdec", "vde2_vdec_active"),
	GATE_VOTE_VDE20(CLK_VDE2_VDEC_CKEN_ENG, "vde2_vdec_cken_eng", "ck2_vdec_ck", 8),
	GATE_VDE20_V(CLK_VDE2_VDEC_CKEN_ENG_VDEC, "vde2_vdec_cken_eng_vdec", "vde2_vdec_cken_eng"),
	/* VDE21 */
	GATE_VOTE_VDE21(CLK_VDE2_LAT_CKEN, "vde2_lat_cken", "ck2_vdec_ck", 0),
	GATE_VDE21_V(CLK_VDE2_LAT_CKEN_VDEC, "vde2_lat_cken_vdec", "vde2_lat_cken"),
	GATE_VOTE_VDE21(CLK_VDE2_LAT_ACTIVE, "vde2_lat_active", "ck2_vdec_ck", 4),
	GATE_VDE21_V(CLK_VDE2_LAT_ACTIVE_VDEC, "vde2_lat_active_vdec", "vde2_lat_active"),
	GATE_VOTE_VDE21(CLK_VDE2_LAT_CKEN_ENG, "vde2_lat_cken_eng", "ck2_vdec_ck", 8),
	GATE_VDE21_V(CLK_VDE2_LAT_CKEN_ENG_VDEC, "vde2_lat_cken_eng_vdec", "vde2_lat_cken_eng"),
	/* VDE22 */
	GATE_VOTE_VDE22(CLK_VDE2_LARB1_CKEN, "vde2_larb1_cken", "ck2_vdec_ck", 0),
	GATE_VDE22_V(CLK_VDE2_LARB1_CKEN_VDEC, "vde2_larb1_cken_vdec", "vde2_larb1_cken"),
	GATE_VDE22_V(CLK_VDE2_LARB1_CKEN_SMI, "vde2_larb1_cken_smi", "vde2_larb1_cken"),
};

static const struct mtk_clk_desc vde2_mcd = {
	.clks = vde2_clks,
	.num_clks = ARRAY_SIZE(vde2_clks),
	.need_runtime_pm = true,
};

static const struct mtk_gate_regs vde10_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x4,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs vde10_vote_regs = {
	.set_ofs = 0x00a0,
	.clr_ofs = 0x00a4,
	.sta_ofs = 0x2c50,
};

static const struct mtk_gate_regs vde11_cg_regs = {
	.set_ofs = 0x1e0,
	.clr_ofs = 0x1e0,
	.sta_ofs = 0x1e0,
};

static const struct mtk_gate_regs vde11_vote_regs = {
	.set_ofs = 0x00b0,
	.clr_ofs = 0x00b4,
	.sta_ofs = 0x2c58,
};

static const struct mtk_gate_regs vde12_cg_regs = {
	.set_ofs = 0x1ec,
	.clr_ofs = 0x1ec,
	.sta_ofs = 0x1ec,
};

static const struct mtk_gate_regs vde12_vote_regs = {
	.set_ofs = 0x00a8,
	.clr_ofs = 0x00ac,
	.sta_ofs = 0x2c54,
};

static const struct mtk_gate_regs vde13_cg_regs = {
	.set_ofs = 0x200,
	.clr_ofs = 0x204,
	.sta_ofs = 0x200,
};

static const struct mtk_gate_regs vde13_vote_regs = {
	.set_ofs = 0x0098,
	.clr_ofs = 0x009c,
	.sta_ofs = 0x2c4c,
};

static const struct mtk_gate_regs vde14_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0xc,
	.sta_ofs = 0x8,
};

static const struct mtk_gate_regs vde14_vote_regs = {
	.set_ofs = 0x0090,
	.clr_ofs = 0x0094,
	.sta_ofs = 0x2c48,
};

#define GATE_VDE10(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde10_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE10_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE10(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &vde10_cg_regs,			\
		.vote_regs = &vde10_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VDE11(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde11_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE		\
		.ops = &mtk_clk_gate_ops_no_setclr_inv,	\
	}

#define GATE_VDE11_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE11(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.vote_comp = "mm-vote-regmap",			\
		.regs = &vde11_cg_regs,				\
		.vote_regs = &vde11_vote_regs,			\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_vote_inv,		\
		.dma_ops = &mtk_clk_gate_ops_no_setclr_inv,	\
		.flags = CLK_USE_VOTE |				\
			 CLK_OPS_PARENT_ENABLE,			\
	}

#define GATE_VDE12(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde12_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_no_setclr_inv,	\
	}

#define GATE_VDE12_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE12(_id, _name, _parent, _shift) {		\
		.id = _id,					\
		.name = _name,					\
		.parent_name = _parent,				\
		.vote_comp = "mm-vote-regmap",			\
		.regs = &vde12_cg_regs,				\
		.vote_regs = &vde12_vote_regs,			\
		.shift = _shift,				\
		.ops = &mtk_clk_gate_ops_vote_inv,		\
		.dma_ops = &mtk_clk_gate_ops_no_setclr_inv,	\
		.flags = CLK_USE_VOTE |				\
			 CLK_OPS_PARENT_ENABLE			\
	}

#define GATE_VDE13(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde13_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE13_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE13(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &vde13_cg_regs,			\
		.vote_regs = &vde13_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VDE14(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vde14_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VDE14_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VDE14(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &vde14_cg_regs,			\
		.vote_regs = &vde14_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE |	\
			 CLK_IGNORE_UNUSED,		\
	}

static const struct mtk_gate vde1_clks[] = {
	/* VDE10 */
	GATE_VOTE_VDE10(CLK_VDE1_VDEC_CKEN, "vde1_vdec_cken", "ck2_vdec_ck", 0),
	GATE_VDE10_V(CLK_VDE1_VDEC_CKEN_VDEC, "vde1_vdec_cken_vdec", "vde1_vdec_cken"),
	GATE_VOTE_VDE10(CLK_VDE1_VDEC_ACTIVE, "vde1_vdec_active", "ck2_vdec_ck", 4),
	GATE_VDE10_V(CLK_VDE1_VDEC_ACTIVE_VDEC, "vde1_vdec_active_vdec", "vde1_vdec_active"),
	GATE_VOTE_VDE10(CLK_VDE1_VDEC_CKEN_ENG, "vde1_vdec_cken_eng", "ck2_vdec_ck", 8),
	GATE_VDE10_V(CLK_VDE1_VDEC_CKEN_ENG_VDEC, "vde1_vdec_cken_eng_vdec", "vde1_vdec_cken_eng"),
	/* VDE11 */
	GATE_VOTE_VDE11(CLK_VDE1_VDEC_SOC_IPS_EN, "vde1_vdec_soc_ips_en", "ck2_vdec_ck", 0),
	GATE_VDE11_V(CLK_VDE1_VDEC_SOC_IPS_EN_VDEC, "vde1_vdec_soc_ips_en_vdec",
		     "vde1_vdec_soc_ips_en"),
	/* VDE12 */
	GATE_VOTE_VDE12(CLK_VDE1_VDEC_SOC_APTV_EN, "vde1_aptv_en", "ck2_avs_vdec_ck", 0),
	GATE_VDE12_V(CLK_VDE1_VDEC_SOC_APTV_EN_VDEC, "vde1_aptv_en_vdec", "vde1_aptv_en"),
	GATE_VOTE_VDE12(CLK_VDE1_VDEC_SOC_APTV_TOP_EN, "vde1_aptv_topen", "ck2_avs_vdec_ck", 1),
	GATE_VDE12_V(CLK_VDE1_VDEC_SOC_APTV_TOP_EN_VDEC, "vde1_aptv_topen_vdec", "vde1_aptv_topen"),
	/* VDE13 */
	GATE_VOTE_VDE13(CLK_VDE1_LAT_CKEN, "vde1_lat_cken", "ck2_vdec_ck", 0),
	GATE_VDE13_V(CLK_VDE1_LAT_CKEN_VDEC, "vde1_lat_cken_vdec", "vde1_lat_cken"),
	GATE_VOTE_VDE13(CLK_VDE1_LAT_ACTIVE, "vde1_lat_active", "ck2_vdec_ck", 4),
	GATE_VDE13_V(CLK_VDE1_LAT_ACTIVE_VDEC, "vde1_lat_active_vdec", "vde1_lat_active"),
	GATE_VOTE_VDE13(CLK_VDE1_LAT_CKEN_ENG, "vde1_lat_cken_eng", "ck2_vdec_ck", 8),
	GATE_VDE13_V(CLK_VDE1_LAT_CKEN_ENG_VDEC, "vde1_lat_cken_eng_vdec", "vde1_lat_cken_eng"),
	/* VDE14 */
	GATE_VOTE_VDE14(CLK_VDE1_LARB1_CKEN, "vde1_larb1_cken", "ck2_vdec_ck", 0),
	GATE_VDE14_V(CLK_VDE1_LARB1_CKEN_VDEC, "vde1_larb1_cken_vdec", "vde1_larb1_cken"),
	GATE_VDE14_V(CLK_VDE1_LARB1_CKEN_SMI, "vde1_larb1_cken_smi", "vde1_larb1_cken"),
};

static const struct mtk_clk_desc vde1_mcd = {
	.clks = vde1_clks,
	.num_clks = ARRAY_SIZE(vde1_clks),
	.need_runtime_pm = true,
};

static const struct of_device_id of_match_clk_mt8196_vdec[] = {
	{ .compatible = "mediatek,mt8196-vdecsys", .data = &vde2_mcd, },
	{ .compatible = "mediatek,mt8196-vdecsys_soc", .data = &vde1_mcd, },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_vdec_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-vdec",
		.of_match_table = of_match_clk_mt8196_vdec,
	},
};

module_platform_driver(clk_mt8196_vdec_drv);
MODULE_LICENSE("GPL");
