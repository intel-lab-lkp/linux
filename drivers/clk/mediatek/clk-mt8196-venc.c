// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 * Author: Guangjie Song <guangjie.song@mediatek.com>
 */
#include <dt-bindings/clock/mt8196-clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

static const struct mtk_gate_regs ven10_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs ven10_vote_regs = {
	.set_ofs = 0x00b8,
	.clr_ofs = 0x00bc,
	.sta_ofs = 0x2c5c,
};

static const struct mtk_gate_regs ven11_cg_regs = {
	.set_ofs = 0x10,
	.clr_ofs = 0x14,
	.sta_ofs = 0x10,
};

static const struct mtk_gate_regs ven11_vote_regs = {
	.set_ofs = 0x00c0,
	.clr_ofs = 0x00c4,
	.sta_ofs = 0x2c60,
};

#define GATE_VEN10(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven10_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VEN10_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VEN10_FLAGS(_id, _name, _parent, _shift, _flags) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ven10_cg_regs,			\
		.vote_regs = &ven10_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = (_flags) |			\
			 CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VOTE_VEN10(_id, _name, _parent, _shift)	\
	GATE_VOTE_VEN10_FLAGS(_id, _name, _parent, _shift, 0)

#define GATE_VEN11(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven11_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_VEN11_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VEN11(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ven11_cg_regs,			\
		.vote_regs = &ven11_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE		\
	}

static const struct mtk_gate ven1_clks[] = {
	/* VEN10 */
	GATE_VOTE_VEN10(CLK_VEN1_CKE0_LARB, "ven1_larb", "ck2_venc_ck", 0),
	GATE_VEN10_V(CLK_VEN1_CKE0_LARB_VENC, "ven1_larb_venc", "ven1_larb"),
	GATE_VEN10_V(CLK_VEN1_CKE0_LARB_JPGENC, "ven1_larb_jpgenc", "ven1_larb"),
	GATE_VEN10_V(CLK_VEN1_CKE0_LARB_JPGDEC, "ven1_larb_jpgdec", "ven1_larb"),
	GATE_VEN10_V(CLK_VEN1_CKE0_LARB_SMI, "ven1_larb_smi", "ven1_larb"),
	GATE_VOTE_VEN10(CLK_VEN1_CKE1_VENC, "ven1_venc", "ck2_venc_ck", 4),
	GATE_VEN10_V(CLK_VEN1_CKE1_VENC_VENC, "ven1_venc_venc", "ven1_venc"),
	GATE_VEN10_V(CLK_VEN1_CKE1_VENC_SMI, "ven1_venc_smi", "ven1_venc"),
	GATE_VEN10(CLK_VEN1_CKE2_JPGENC, "ven1_jpgenc", "ck2_venc_ck", 8),
	GATE_VEN10_V(CLK_VEN1_CKE2_JPGENC_JPGENC, "ven1_jpgenc_jpgenc", "ven1_jpgenc"),
	GATE_VEN10(CLK_VEN1_CKE3_JPGDEC, "ven1_jpgdec", "ck2_venc_ck", 12),
	GATE_VEN10_V(CLK_VEN1_CKE3_JPGDEC_JPGDEC, "ven1_jpgdec_jpgdec", "ven1_jpgdec"),
	GATE_VEN10(CLK_VEN1_CKE4_JPGDEC_C1, "ven1_jpgdec_c1", "ck2_venc_ck", 16),
	GATE_VEN10_V(CLK_VEN1_CKE4_JPGDEC_C1_JPGDEC, "ven1_jpgdec_c1_jpgdec", "ven1_jpgdec_c1"),
	GATE_VOTE_VEN10(CLK_VEN1_CKE5_GALS, "ven1_gals", "ck2_venc_ck", 28),
	GATE_VEN10_V(CLK_VEN1_CKE5_GALS_VENC, "ven1_gals_venc", "ven1_gals"),
	GATE_VEN10_V(CLK_VEN1_CKE5_GALS_JPGENC, "ven1_gals_jpgenc", "ven1_gals"),
	GATE_VEN10_V(CLK_VEN1_CKE5_GALS_JPGDEC, "ven1_gals_jpgdec", "ven1_gals"),
	GATE_VOTE_VEN10(CLK_VEN1_CKE29_VENC_ADAB_CTRL, "ven1_venc_adab_ctrl", "ck2_venc_ck", 29),
	GATE_VEN10_V(CLK_VEN1_CKE29_VENC_ADAB_CTRL_VENC, "ven1_venc_adab_ctrl_venc",
		     "ven1_venc_adab_ctrl"),
	GATE_VOTE_VEN10_FLAGS(CLK_VEN1_CKE29_VENC_XPC_CTRL, "ven1_venc_xpc_ctrl",
			      "ck2_venc_ck", 30, CLK_IGNORE_UNUSED),
	GATE_VEN10_V(CLK_VEN1_CKE29_VENC_XPC_CTRL_VENC, "ven1_venc_xpc_ctrl_venc",
		     "ven1_venc_xpc_ctrl"),
	GATE_VEN10_V(CLK_VEN1_CKE29_VENC_XPC_CTRL_JPGENC, "ven1_venc_xpc_ctrl_jpgenc",
		     "ven1_venc_xpc_ctrl"),
	GATE_VEN10_V(CLK_VEN1_CKE29_VENC_XPC_CTRL_JPGDEC, "ven1_venc_xpc_ctrl_jpgdec",
		     "ven1_venc_xpc_ctrl"),
	GATE_VOTE_VEN10(CLK_VEN1_CKE6_GALS_SRAM, "ven1_gals_sram", "ck2_venc_ck", 31),
	GATE_VEN10_V(CLK_VEN1_CKE6_GALS_SRAM_VENC, "ven1_gals_sram_venc", "ven1_gals_sram"),
	/* VEN11 */
	GATE_VOTE_VEN11(CLK_VEN1_RES_FLAT, "ven1_res_flat", "ck2_venc_ck", 0),
	GATE_VEN11_V(CLK_VEN1_RES_FLAT_VENC, "ven1_res_flat_venc", "ven1_res_flat"),
	GATE_VEN11_V(CLK_VEN1_RES_FLAT_JPGENC, "ven1_res_flat_jpgenc", "ven1_res_flat"),
	GATE_VEN11_V(CLK_VEN1_RES_FLAT_JPGDEC, "ven1_res_flat_jpgdec", "ven1_res_flat"),
};

static const struct mtk_clk_desc ven1_mcd = {
	.clks = ven1_clks,
	.num_clks = ARRAY_SIZE(ven1_clks),
	.need_runtime_pm = true,
};

static const struct mtk_gate_regs ven20_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs ven20_vote_regs = {
	.set_ofs = 0x00c8,
	.clr_ofs = 0x00cc,
	.sta_ofs = 0x2c64,
};

static const struct mtk_gate_regs ven21_cg_regs = {
	.set_ofs = 0x10,
	.clr_ofs = 0x14,
	.sta_ofs = 0x10,
};

static const struct mtk_gate_regs ven21_vote_regs = {
	.set_ofs = 0x00d0,
	.clr_ofs = 0x00d4,
	.sta_ofs = 0x2c68,
};

#define GATE_VEN20(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven20_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VEN20_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VEN20(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ven20_cg_regs,			\
		.vote_regs = &ven20_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VEN21(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven21_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_VEN21_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VEN21(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ven21_cg_regs,			\
		.vote_regs = &ven21_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE		\
	}

static const struct mtk_gate ven2_clks[] = {
	/* VEN20 */
	GATE_VOTE_VEN20(CLK_VEN2_CKE0_LARB, "ven2_larb", "ck2_venc_ck", 0),
	GATE_VEN20_V(CLK_VEN2_CKE0_LARB_VENC, "ven2_larb_venc", "ven2_larb"),
	GATE_VEN20_V(CLK_VEN2_CKE0_LARB_JPGENC, "ven2_larb_jpgenc", "ven2_larb"),
	GATE_VEN20_V(CLK_VEN2_CKE0_LARB_JPGDEC, "ven2_larb_jpgdec", "ven2_larb"),
	GATE_VEN20_V(CLK_VEN2_CKE0_LARB_SMI, "ven2_larb_smi", "ven2_larb"),
	GATE_VOTE_VEN20(CLK_VEN2_CKE1_VENC, "ven2_venc", "ck2_venc_ck", 4),
	GATE_VEN20_V(CLK_VEN2_CKE1_VENC_VENC, "ven2_venc_venc", "ven2_venc"),
	GATE_VEN20_V(CLK_VEN2_CKE1_VENC_SMI, "ven2_venc_smi", "ven2_venc"),
	GATE_VEN20(CLK_VEN2_CKE2_JPGENC, "ven2_jpgenc", "ck2_venc_ck", 8),
	GATE_VEN20_V(CLK_VEN2_CKE2_JPGENC_JPGENC, "ven2_jpgenc_jpgenc", "ven2_jpgenc"),
	GATE_VEN20(CLK_VEN2_CKE3_JPGDEC, "ven2_jpgdec", "ck2_venc_ck", 12),
	GATE_VEN20_V(CLK_VEN2_CKE3_JPGDEC_JPGDEC, "ven2_jpgdec_jpgdec", "ven2_jpgdec"),
	GATE_VOTE_VEN20(CLK_VEN2_CKE5_GALS, "ven2_gals", "ck2_venc_ck", 28),
	GATE_VEN20_V(CLK_VEN2_CKE5_GALS_VENC, "ven2_gals_venc", "ven2_gals"),
	GATE_VEN20_V(CLK_VEN2_CKE5_GALS_JPGENC, "ven2_gals_jpgenc", "ven2_gals"),
	GATE_VEN20_V(CLK_VEN2_CKE5_GALS_JPGDEC, "ven2_gals_jpgdec", "ven2_gals"),
	GATE_VOTE_VEN20(CLK_VEN2_CKE29_VENC_XPC_CTRL, "ven2_venc_xpc_ctrl", "ck2_venc_ck", 30),
	GATE_VEN20_V(CLK_VEN2_CKE29_VENC_XPC_CTRL_VENC, "ven2_venc_xpc_ctrl_venc",
		     "ven2_venc_xpc_ctrl"),
	GATE_VEN20_V(CLK_VEN2_CKE29_VENC_XPC_CTRL_JPGENC, "ven2_venc_xpc_ctrl_jpgenc",
		     "ven2_venc_xpc_ctrl"),
	GATE_VEN20_V(CLK_VEN2_CKE29_VENC_XPC_CTRL_JPGDEC, "ven2_venc_xpc_ctrl_jpgdec",
		     "ven2_venc_xpc_ctrl"),
	GATE_VOTE_VEN20(CLK_VEN2_CKE6_GALS_SRAM, "ven2_gals_sram", "ck2_venc_ck", 31),
	GATE_VEN20_V(CLK_VEN2_CKE6_GALS_SRAM_VENC, "ven2_gals_sram_venc", "ven2_gals_sram"),
	/* VEN21 */
	GATE_VOTE_VEN21(CLK_VEN2_RES_FLAT, "ven2_res_flat", "ck2_venc_ck", 0),
	GATE_VEN21_V(CLK_VEN2_RES_FLAT_VENC, "ven2_res_flat_venc", "ven2_res_flat"),
	GATE_VEN21_V(CLK_VEN2_RES_FLAT_JPGENC, "ven2_res_flat_jpgenc", "ven2_res_flat"),
	GATE_VEN21_V(CLK_VEN2_RES_FLAT_JPGDEC, "ven2_res_flat_jpgdec", "ven2_res_flat"),
};

static const struct mtk_clk_desc ven2_mcd = {
	.clks = ven2_clks,
	.num_clks = ARRAY_SIZE(ven2_clks),
	.need_runtime_pm = true,
};

static const struct mtk_gate_regs ven_c20_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs ven_c20_vote_regs = {
	.set_ofs = 0x00d8,
	.clr_ofs = 0x00dc,
	.sta_ofs = 0x2c6c,
};

static const struct mtk_gate_regs ven_c21_cg_regs = {
	.set_ofs = 0x10,
	.clr_ofs = 0x14,
	.sta_ofs = 0x10,
};

static const struct mtk_gate_regs ven_c21_vote_regs = {
	.set_ofs = 0x00e0,
	.clr_ofs = 0x00e4,
	.sta_ofs = 0x2c70,
};

#define GATE_VEN_C20(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven_c20_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr_inv,	\
	}

#define GATE_VEN_C20_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VEN_C20(_id, _name, _parent, _shift) {\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ven_c20_cg_regs,		\
		.vote_regs = &ven_c20_vote_regs,	\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote_inv,	\
		.dma_ops = &mtk_clk_gate_ops_setclr_inv,\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VEN_C21(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ven_c21_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_VEN_C21_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_VEN_C21(_id, _name, _parent, _shift) {\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &ven_c21_cg_regs,		\
		.vote_regs = &ven_c21_vote_regs,	\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

static const struct mtk_gate ven_c2_clks[] = {
	/* VEN_C20 */
	GATE_VOTE_VEN_C20(CLK_VEN_C2_CKE0_LARB, "ven_c2_larb", "ck2_venc_ck", 0),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE0_LARB_VENC, "ven_c2_larb_venc", "ven_c2_larb"),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE0_LARB_SMI, "ven_c2_larb_smi", "ven_c2_larb"),
	GATE_VOTE_VEN_C20(CLK_VEN_C2_CKE1_VENC, "ven_c2_venc", "ck2_venc_ck", 4),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE1_VENC_VENC, "ven_c2_venc_venc", "ven_c2_venc"),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE1_VENC_SMI, "ven_c2_venc_smi", "ven_c2_venc"),
	GATE_VOTE_VEN_C20(CLK_VEN_C2_CKE5_GALS, "ven_c2_gals", "ck2_venc_ck", 28),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE5_GALS_VENC, "ven_c2_gals_venc", "ven_c2_gals"),
	GATE_VOTE_VEN_C20(CLK_VEN_C2_CKE29_VENC_XPC_CTRL, "ven_c2_venc_xpc_ctrl",
			  "ck2_venc_ck", 30),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE29_VENC_XPC_CTRL_VENC, "ven_c2_venc_xpc_ctrl_venc",
		       "ven_c2_venc_xpc_ctrl"),
	GATE_VOTE_VEN_C20(CLK_VEN_C2_CKE6_GALS_SRAM, "ven_c2_gals_sram", "ck2_venc_ck", 31),
	GATE_VEN_C20_V(CLK_VEN_C2_CKE6_GALS_SRAM_VENC, "ven_c2_gals_sram_venc", "ven_c2_gals_sram"),
	/* VEN_C21 */
	GATE_VOTE_VEN_C21(CLK_VEN_C2_RES_FLAT, "ven_c2_res_flat", "ck2_venc_ck", 0),
	GATE_VEN_C21_V(CLK_VEN_C2_RES_FLAT_VENC, "ven_c2_res_flat_venc", "ven_c2_res_flat"),
};

static const struct mtk_clk_desc ven_c2_mcd = {
	.clks = ven_c2_clks,
	.num_clks = ARRAY_SIZE(ven_c2_clks),
	.need_runtime_pm = true,
};

static const struct of_device_id of_match_clk_mt8196_venc[] = {
	{ .compatible = "mediatek,mt8196-vencsys", .data = &ven1_mcd, },
	{ .compatible = "mediatek,mt8196-vencsys_c1", .data = &ven2_mcd, },
	{ .compatible = "mediatek,mt8196-vencsys_c2", .data = &ven_c2_mcd, },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_venc_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-venc",
		.of_match_table = of_match_clk_mt8196_venc,
	},
};

module_platform_driver(clk_mt8196_venc_drv);
MODULE_LICENSE("GPL");
