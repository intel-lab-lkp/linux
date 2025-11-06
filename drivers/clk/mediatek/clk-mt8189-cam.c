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

static const struct mtk_gate_regs cam_m_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_CAM_M(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &cam_m_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_OPS_PARENT_ENABLE | CLK_IGNORE_UNUSED,	\
	}

static const struct mtk_gate cam_m_clks[] = {
	GATE_CAM_M(CLK_CAM_M_LARB13, "cam_m_larb13", "cam_sel", 0),
	GATE_CAM_M(CLK_CAM_M_LARB14, "cam_m_larb14", "cam_sel", 2),
	GATE_CAM_M(CLK_CAM_M_CAMSYS_MAIN_CAM, "cam_m_camsys_main_cam", "cam_sel", 6),
	GATE_CAM_M(CLK_CAM_M_CAMSYS_MAIN_CAMTG, "cam_m_camsys_main_camtg", "cam_sel", 7),
	GATE_CAM_M(CLK_CAM_M_SENINF, "cam_m_seninf", "cam_sel", 8),
	GATE_CAM_M(CLK_CAM_M_CAMSV1, "cam_m_camsv1", "cam_sel", 10),
	GATE_CAM_M(CLK_CAM_M_CAMSV2, "cam_m_camsv2", "cam_sel", 11),
	GATE_CAM_M(CLK_CAM_M_CAMSV3, "cam_m_camsv3", "cam_sel", 12),
	GATE_CAM_M(CLK_CAM_M_FAKE_ENG, "cam_m_fake_eng", "cam_sel", 17),
	GATE_CAM_M(CLK_CAM_M_CAM2MM_GALS, "cam_m_cam2mm_gals", "cam_sel", 19),
	GATE_CAM_M(CLK_CAM_M_CAMSV4, "cam_m_camsv4", "cam_sel", 20),
	GATE_CAM_M(CLK_CAM_M_PDA, "cam_m_pda", "cam_sel", 21),
};

static const struct mtk_clk_desc cam_m_mcd = {
	.clks = cam_m_clks,
	.num_clks = ARRAY_SIZE(cam_m_clks),
};

static const struct mtk_gate_regs cam_ra_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_CAM_RA(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &cam_ra_cg_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_OPS_PARENT_ENABLE | CLK_IGNORE_UNUSED,	\
	}

static const struct mtk_gate cam_ra_clks[] = {
	GATE_CAM_RA(CLK_CAM_RA_CAMSYS_RAWA_LARBX, "cam_ra_camsys_rawa_larbx", "cam_sel", 0),
	GATE_CAM_RA(CLK_CAM_RA_CAMSYS_RAWA_CAM, "cam_ra_camsys_rawa_cam", "cam_sel", 1),
	GATE_CAM_RA(CLK_CAM_RA_CAMSYS_RAWA_CAMTG, "cam_ra_camsys_rawa_camtg", "cam_sel", 2),
};

static const struct mtk_clk_desc cam_ra_mcd = {
	.clks = cam_ra_clks,
	.num_clks = ARRAY_SIZE(cam_ra_clks),
};

static const struct mtk_gate_regs cam_rb_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_CAM_RB(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &cam_rb_cg_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
		.flags = CLK_OPS_PARENT_ENABLE | CLK_IGNORE_UNUSED,	\
	}

static const struct mtk_gate cam_rb_clks[] = {
	GATE_CAM_RB(CLK_CAM_RB_CAMSYS_RAWB_LARBX, "cam_rb_camsys_rawb_larbx", "cam_sel", 0),
	GATE_CAM_RB(CLK_CAM_RB_CAMSYS_RAWB_CAM, "cam_rb_camsys_rawb_cam", "cam_sel", 1),
	GATE_CAM_RB(CLK_CAM_RB_CAMSYS_RAWB_CAMTG, "cam_rb_camsys_rawb_camtg", "cam_sel", 2),
};

static const struct mtk_clk_desc cam_rb_mcd = {
	.clks = cam_rb_clks,
	.num_clks = ARRAY_SIZE(cam_rb_clks),
};

static const struct of_device_id of_match_clk_mt8189_cam[] = {
	{ .compatible = "mediatek,mt8189-camsys-main", .data = &cam_m_mcd },
	{ .compatible = "mediatek,mt8189-camsys-rawa", .data = &cam_ra_mcd },
	{ .compatible = "mediatek,mt8189-camsys-rawb", .data = &cam_rb_mcd },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8189_cam_drv = {
	.probe = mtk_clk_simple_probe,
	.driver = {
		.name = "clk-mt8189-cam",
		.of_match_table = of_match_clk_mt8189_cam,
	},
};

module_platform_driver(clk_mt8189_cam_drv);
MODULE_LICENSE("GPL");
