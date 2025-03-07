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

static const struct mtk_gate_regs mm_v_cg_regs = {
	.set_ofs = 0x104,
	.clr_ofs = 0x108,
	.sta_ofs = 0x100,
};

static const struct mtk_gate_regs mm_v_vote_regs = {
	.set_ofs = 0x0030,
	.clr_ofs = 0x0034,
	.sta_ofs = 0x2c18,
};

#define GATE_MM_V(_id, _name, _parent, _shift) {\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &mm_v_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_MM_V_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_MM_AO_V(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &mm_v_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr_enable,	\
		.flags = CLK_OPS_PARENT_ENABLE,		\
	}

#define GATE_VOTE_MM_V(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.vote_comp = "mm-vote-regmap",		\
		.regs = &mm_v_cg_regs,			\
		.vote_regs = &mm_v_vote_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_vote,		\
		.dma_ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_USE_VOTE |			\
			 CLK_OPS_PARENT_ENABLE,		\
	}

static const struct mtk_gate mm_v_clks[] = {
	GATE_VOTE_MM_V(CLK_MM_V_DISP_VDISP_AO_CONFIG, "mm_v_disp_vdisp_ao_config", "ck2_disp_ck", 0),
	GATE_MM_V_V(CLK_MM_V_DISP_VDISP_AO_CONFIG_DISP, "mm_v_disp_vdisp_ao_config_disp",
		    "mm_v_disp_vdisp_ao_config"),
	GATE_VOTE_MM_V(CLK_MM_V_DISP_DPC, "mm_v_disp_dpc", "ck2_disp_ck", 16),
	GATE_MM_V_V(CLK_MM_V_DISP_DPC_DISP, "mm_v_disp_dpc_disp", "mm_v_disp_dpc"),
	GATE_MM_AO_V(CLK_MM_V_SMI_SUB_SOMM0, "mm_v_smi_sub_somm0", "ck2_disp_ck", 2),
	GATE_MM_V_V(CLK_MM_V_SMI_SUB_SOMM0_SMI, "mm_v_smi_sub_somm0_smi", "mm_v_smi_sub_somm0"),
};

static const struct mtk_clk_desc mm_v_mcd = {
	.clks = mm_v_clks,
	.num_clks = ARRAY_SIZE(mm_v_clks),
};

static const struct platform_device_id clk_mt8196_vdisp_ao_id_table[] = {
	{ .name = "clk-mt8196-vdisp_ao", .driver_data = (kernel_ulong_t)&mm_v_mcd },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, clk_mt8196_vdisp_ao_id_table);

static struct platform_driver clk_mt8196_vdisp_ao_drv = {
	.probe = mtk_clk_pdev_probe,
	.remove = mtk_clk_pdev_remove,
	.driver = {
		.name = "clk-mt8196-vdisp_ao",
	},
	.id_table = clk_mt8196_vdisp_ao_id_table,
};

module_platform_driver(clk_mt8196_vdisp_ao_drv);
MODULE_LICENSE("GPL");
