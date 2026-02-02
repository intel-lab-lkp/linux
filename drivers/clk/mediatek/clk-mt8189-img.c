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

static const struct mtk_gate_regs imgsys1_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_IMGSYS1(_id, _name, _parent, _shift)			\
	GATE_MTK_FLAGS(_id, _name, _parent, &imgsys1_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_IGNORE_UNUSED)

static const struct mtk_gate imgsys1_clks[] = {
	GATE_IMGSYS1(CLK_IMGSYS1_LARB9, "imgsys1_larb9", "img1_sel", 0),
	GATE_IMGSYS1(CLK_IMGSYS1_LARB11, "imgsys1_larb11", "img1_sel", 1),
	GATE_IMGSYS1(CLK_IMGSYS1_DIP, "imgsys1_dip", "img1_sel", 2),
	GATE_IMGSYS1(CLK_IMGSYS1_GALS, "imgsys1_gals", "img1_sel", 12),
};

static const struct mtk_clk_desc imgsys1_mcd = {
	.clks = imgsys1_clks,
	.num_clks = ARRAY_SIZE(imgsys1_clks),
};

static const struct mtk_gate_regs imgsys2_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_IMGSYS2(_id, _name, _parent, _shift)			\
	GATE_MTK_FLAGS(_id, _name, _parent, &imgsys2_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_IGNORE_UNUSED)

static const struct mtk_gate imgsys2_clks[] = {
	GATE_IMGSYS2(CLK_IMGSYS2_LARB9, "imgsys2_larb9", "img1_sel", 0),
	GATE_IMGSYS2(CLK_IMGSYS2_LARB11, "imgsys2_larb11", "img1_sel", 1),
	GATE_IMGSYS2(CLK_IMGSYS2_MFB, "imgsys2_mfb", "img1_sel", 6),
	GATE_IMGSYS2(CLK_IMGSYS2_WPE, "imgsys2_wpe", "img1_sel", 7),
	GATE_IMGSYS2(CLK_IMGSYS2_MSS, "imgsys2_mss", "img1_sel", 8),
	GATE_IMGSYS2(CLK_IMGSYS2_GALS, "imgsys2_gals", "img1_sel", 12),
};

static const struct mtk_clk_desc imgsys2_mcd = {
	.clks = imgsys2_clks,
	.num_clks = ARRAY_SIZE(imgsys2_clks),
};

static const struct mtk_gate_regs ipe_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_IPE(_id, _name, _parent, _shift)				\
	GATE_MTK_FLAGS(_id, _name, _parent, &ipe_cg_regs, _shift,	\
		       &mtk_clk_gate_ops_setclr, CLK_IGNORE_UNUSED)

static const struct mtk_gate ipe_clks[] = {
	GATE_IPE(CLK_IPE_LARB19, "ipe_larb19", "ipe_sel", 0),
	GATE_IPE(CLK_IPE_LARB20, "ipe_larb20", "ipe_sel", 1),
	GATE_IPE(CLK_IPE_SMI_SUBCOM, "ipe_smi_subcom", "ipe_sel", 2),
	GATE_IPE(CLK_IPE_FD, "ipe_fd", "ipe_sel", 3),
	GATE_IPE(CLK_IPE_FE, "ipe_fe", "ipe_sel", 4),
	GATE_IPE(CLK_IPE_RSC, "ipe_rsc", "ipe_sel", 5),
	GATE_IPE(CLK_IPESYS_GALS, "ipesys_gals", "ipe_sel", 8),
};

static const struct mtk_clk_desc ipe_mcd = {
	.clks = ipe_clks,
	.num_clks = ARRAY_SIZE(ipe_clks),
};

static const struct of_device_id of_match_clk_mt8189_img[] = {
	{ .compatible = "mediatek,mt8189-imgsys1", .data = &imgsys1_mcd },
	{ .compatible = "mediatek,mt8189-imgsys2", .data = &imgsys2_mcd },
	{ .compatible = "mediatek,mt8189-ipesys", .data = &ipe_mcd },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt8189_img);

static struct platform_driver clk_mt8189_img_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8189-img",
		.of_match_table = of_match_clk_mt8189_img,
	},
};

module_platform_driver(clk_mt8189_img_drv);
MODULE_DESCRIPTION("MediaTek MT8189 img clocks driver");
MODULE_LICENSE("GPL");
