// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 MediaTek Inc.
 * Copyright (c) 2025 Collabora Ltd
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <dt-bindings/clock/mediatek,mt6893-clk.h>

#include "clk-mtk.h"
#include "clk-gate.h"

static const struct mtk_gate_regs venc_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x8,
	.sta_ofs = 0x0,
};

#define GATE_VENC(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &venc_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate venc0_clks[] = {
	GATE_VENC(CLK_VENC0_SET0_LARB, "venc0_set0_larb", "venc_sel", 0),
	GATE_VENC(CLK_VENC0_SET1_VENC, "venc0_set1_venc", "venc_sel", 4),
	GATE_VENC(CLK_VENC0_SET2_JPGENC, "venc0_set2_jpgenc", "venc_sel", 8),
	GATE_VENC(CLK_VENC0_SET3_JPGDEC, "venc0_set2_jpgdec", "venc_sel", 12),
	GATE_VENC(CLK_VENC0_SET4_JPGDEC_C1, "venc0_set2_jpgdec_c1", "venc_sel", 16),
	GATE_VENC(CLK_VENC0_SET5_GALS, "venc0_set5_gals", "venc_sel", 28),
};

static const struct mtk_clk_desc venc0_desc = {
	.clks = venc0_clks,
	.num_clks = ARRAY_SIZE(venc0_clks),
};

static const struct mtk_gate venc1_clks[] = {
	GATE_VENC(CLK_VENC1_SET0_LARB, "venc1_set0_larb", "venc_sel", 0),
	GATE_VENC(CLK_VENC1_SET1_VENC, "venc1_set1_venc", "venc_sel", 4),
	GATE_VENC(CLK_VENC1_SET2_JPGENC, "venc1_set2_jpgenc", "venc_sel", 8),
	GATE_VENC(CLK_VENC1_SET3_JPGDEC, "venc1_set2_jpgdec", "venc_sel", 12),
	GATE_VENC(CLK_VENC1_SET4_JPGDEC_C1, "venc1_set2_jpgdec_c1", "venc_sel", 16),
	GATE_VENC(CLK_VENC1_SET5_GALS, "venc1_set5_gals", "venc_sel", 28),
};

static const struct mtk_clk_desc venc1_desc = {
	.clks = venc1_clks,
	.num_clks = ARRAY_SIZE(venc1_clks),
};

static const struct of_device_id of_match_clk_mt6893_venc[] = {
	{ .compatible = "mediatek,mt6893-vencsys-c0", .data = &venc0_desc },
	{ .compatible = "mediatek,mt6893-vencsys-c1", .data = &venc1_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_venc);

static struct platform_driver clk_mt6893_venc_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-venc",
		.of_match_table = of_match_clk_mt6893_venc,
	},
};
module_platform_driver(clk_mt6893_venc_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 Video Encoders clocks driver");
MODULE_LICENSE("GPL");
