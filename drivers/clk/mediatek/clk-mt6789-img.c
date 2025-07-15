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

static const struct mtk_gate_regs imgsys_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_IMGSYS(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &imgsys_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

static const struct mtk_gate imgsys_clks[] = {
	GATE_IMGSYS(CLK_IMGSYS_LARB9, "imgsys_larb9", "img_ck", 0),
	GATE_IMGSYS(CLK_IMGSYS_LARB10, "imgsys_larb10", "img_ck", 1),
	GATE_IMGSYS(CLK_IMGSYS_DIP, "imgsys_dip", "img_ck", 2),
	GATE_IMGSYS(CLK_IMGSYS_GALS, "imgsys_gals", "img_ck", 12),
};

static const struct mtk_clk_desc imgsys_desc = {
	.clks = imgsys_clks,
	.num_clks = ARRAY_SIZE(imgsys_clks),
};

static const struct mtk_gate_regs ipe_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_IPE(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ipe_cg_regs,			\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

static const struct mtk_gate ipe_clks[] = {
	GATE_IPE(CLK_IPE_LARB19, "ipe_larb19", "ipe_ck", 0),
	GATE_IPE(CLK_IPE_LARB20, "ipe_larb20", "ipe_ck", 1),
	GATE_IPE(CLK_IPE_SMI_SUBCOM, "ipe_smi_subcom", "ipe_ck", 2),
	GATE_IPE(CLK_IPE_FD, "ipe_fd", "ipe_ck", 3),
	GATE_IPE(CLK_IPE_FE, "ipe_fe", "ipe_ck", 4),
	GATE_IPE(CLK_IPE_RSC, "ipe_rsc", "ipe_ck", 5),
	GATE_IPE(CLK_IPE_DPE, "ipe_dpe", "ipe_ck", 6),
	GATE_IPE(CLK_IPE_GALS, "ipe_gals", "img1_ck", 8),
};

static const struct mtk_clk_desc ipesys_desc = {
	.clks = ipe_clks,
	.num_clks = ARRAY_SIZE(ipe_clks),
};

static const struct of_device_id of_match_clk_mt6789_img[] = {
	{
		.compatible = "mediatek,mt6789-imgsys",
		.data = &imgsys_desc,
	}, {
		.compatible = "mediatek,mt6789-ipesys",
		.data = &ipesys_desc,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6789_img);

static struct platform_driver clk_mt6789_img_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6789-img",
		.of_match_table = of_match_clk_mt6789_img,
	},
};

module_platform_driver(clk_mt6789_img_drv);

MODULE_DESCRIPTION("MediaTek MT6789 imgsys clocks driver");
MODULE_LICENSE("GPL");
