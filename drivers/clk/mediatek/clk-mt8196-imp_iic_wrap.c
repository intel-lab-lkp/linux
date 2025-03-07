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

static const struct mtk_gate_regs impc_cg_regs = {
	.set_ofs = 0xe08,
	.clr_ofs = 0xe04,
	.sta_ofs = 0xe00,
};

#define GATE_IMPC(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &impc_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_IMPC_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate impc_clks[] = {
	GATE_IMPC(CLK_IMPC_I2C11, "impc_i2c11", "ck_i2c_p_ck", 0),
	GATE_IMPC_V(CLK_IMPC_I2C11_I2C, "impc_i2c11_i2c", "impc_i2c11"),
	GATE_IMPC(CLK_IMPC_I2C12, "impc_i2c12", "ck_i2c_p_ck", 1),
	GATE_IMPC_V(CLK_IMPC_I2C12_I2C, "impc_i2c12_i2c", "impc_i2c12"),
	GATE_IMPC(CLK_IMPC_I2C13, "impc_i2c13", "ck_i2c_p_ck", 2),
	GATE_IMPC_V(CLK_IMPC_I2C13_I2C, "impc_i2c13_i2c", "impc_i2c13"),
	GATE_IMPC(CLK_IMPC_I2C14, "impc_i2c14", "ck_i2c_p_ck", 3),
	GATE_IMPC_V(CLK_IMPC_I2C14_I2C, "impc_i2c14_i2c", "impc_i2c14"),
};

static const struct mtk_clk_desc impc_mcd = {
	.clks = impc_clks,
	.num_clks = ARRAY_SIZE(impc_clks),
};

static const struct mtk_gate_regs impe_cg_regs = {
	.set_ofs = 0xe08,
	.clr_ofs = 0xe04,
	.sta_ofs = 0xe00,
};

#define GATE_IMPE(_id, _name, _parent, _shift) {\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &impe_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_IMPE_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate impe_clks[] = {
	GATE_IMPE(CLK_IMPE_I2C5, "impe_i2c5", "ck_i2c_east_ck", 0),
	GATE_IMPE_V(CLK_IMPE_I2C5_I2C, "impe_i2c5_i2c", "impe_i2c5"),
};

static const struct mtk_clk_desc impe_mcd = {
	.clks = impe_clks,
	.num_clks = ARRAY_SIZE(impe_clks),
};

static const struct mtk_gate_regs impn_cg_regs = {
	.set_ofs = 0xe08,
	.clr_ofs = 0xe04,
	.sta_ofs = 0xe00,
};

static const struct mtk_gate_regs impn_vote_regs = {
	.set_ofs = 0x0000,
	.clr_ofs = 0x0004,
	.sta_ofs = 0x2c00,
};

#define GATE_IMPN(_id, _name, _parent, _shift) {\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &impn_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_IMPN_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_VOTE_IMPN(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "vote-regmap",		\
		.regs = &impn_cg_regs,			\
		.vote_regs = &impn_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

static const struct mtk_gate impn_clks[] = {
	GATE_IMPN(CLK_IMPN_I2C1, "impn_i2c1", "ck_i2c_north_ck", 0),
	GATE_IMPN_V(CLK_IMPN_I2C1_I2C, "impn_i2c1_i2c", "impn_i2c1"),
	GATE_IMPN(CLK_IMPN_I2C2, "impn_i2c2", "ck_i2c_north_ck", 1),
	GATE_IMPN_V(CLK_IMPN_I2C2_I2C, "impn_i2c2_i2c", "impn_i2c2"),
	GATE_IMPN(CLK_IMPN_I2C4, "impn_i2c4", "ck_i2c_north_ck", 2),
	GATE_IMPN_V(CLK_IMPN_I2C4_I2C, "impn_i2c4_i2c", "impn_i2c4"),
	GATE_VOTE_IMPN(CLK_IMPN_I2C7, "impn_i2c7", "ck_i2c_north_ck", 3),
	GATE_IMPN_V(CLK_IMPN_I2C7_I2C, "impn_i2c7_i2c", "impn_i2c7"),
	GATE_IMPN(CLK_IMPN_I2C8, "impn_i2c8", "ck_i2c_north_ck", 4),
	GATE_IMPN_V(CLK_IMPN_I2C8_I2C, "impn_i2c8_i2c", "impn_i2c8"),
	GATE_IMPN(CLK_IMPN_I2C9, "impn_i2c9", "ck_i2c_north_ck", 5),
	GATE_IMPN_V(CLK_IMPN_I2C9_I2C, "impn_i2c9_i2c", "impn_i2c9"),
};

static const struct mtk_clk_desc impn_mcd = {
	.clks = impn_clks,
	.num_clks = ARRAY_SIZE(impn_clks),
};

static const struct mtk_gate_regs impw_cg_regs = {
	.set_ofs = 0xe08,
	.clr_ofs = 0xe04,
	.sta_ofs = 0xe00,
};

#define GATE_IMPW(_id, _name, _parent, _shift) {\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &impw_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_IMPW_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate impw_clks[] = {
	GATE_IMPW(CLK_IMPW_I2C0, "impw_i2c0", "ck_i2c_west_ck", 0),
	GATE_IMPW_V(CLK_IMPW_I2C0_I2C, "impw_i2c0_i2c", "impw_i2c0"),
	GATE_IMPW(CLK_IMPW_I2C3, "impw_i2c3", "ck_i2c_west_ck", 1),
	GATE_IMPW_V(CLK_IMPW_I2C3_I2C, "impw_i2c3_i2c", "impw_i2c3"),
	GATE_IMPW(CLK_IMPW_I2C6, "impw_i2c6", "ck_i2c_west_ck", 2),
	GATE_IMPW_V(CLK_IMPW_I2C6_I2C, "impw_i2c6_i2c", "impw_i2c6"),
	GATE_IMPW(CLK_IMPW_I2C10, "impw_i2c10", "ck_i2c_west_ck", 3),
	GATE_IMPW_V(CLK_IMPW_I2C10_I2C, "impw_i2c10_i2c", "impw_i2c10"),
};

static const struct mtk_clk_desc impw_mcd = {
	.clks = impw_clks,
	.num_clks = ARRAY_SIZE(impw_clks),
};

static const struct of_device_id of_match_clk_mt8196_imp_iic_wrap[] = {
	{ .compatible = "mediatek,mt8196-imp_iic_wrap_c", .data = &impc_mcd, },
	{ .compatible = "mediatek,mt8196-imp_iic_wrap_e", .data = &impe_mcd, },
	{ .compatible = "mediatek,mt8196-imp_iic_wrap_n", .data = &impn_mcd, },
	{ .compatible = "mediatek,mt8196-imp_iic_wrap_w", .data = &impw_mcd, },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_imp_iic_wrap_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-imp_iic_wrap",
		.of_match_table = of_match_clk_mt8196_imp_iic_wrap,
	},
};

module_platform_driver(clk_mt8196_imp_iic_wrap_drv);
MODULE_LICENSE("GPL");
