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

static const struct mtk_gate_regs vdec0_cg_regs = {
	.set_ofs = 0x0,
	.clr_ofs = 0x4,
	.sta_ofs = 0x0,
};

static const struct mtk_gate_regs vdec1_cg_regs = {
	.set_ofs = 0x200,
	.clr_ofs = 0x204,
	.sta_ofs = 0x200,
};

static const struct mtk_gate_regs vdec2_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0xc,
	.sta_ofs = 0x8,
};

#define GATE_VDEC0(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &vdec0_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

#define GATE_VDEC1(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &vdec1_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

#define GATE_VDEC2(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &vdec2_cg_regs, _shift, &mtk_clk_gate_ops_setclr_inv)

static const struct mtk_gate vdec_clks[] = {
	/* VDEC0 */
	GATE_VDEC0(CLK_VDEC_VDEC, "vdec_vdec", "vdec_sel", 0),
	GATE_VDEC0(CLK_VDEC_ACTIVE, "vdec_active", "vdec_sel", 4),
	GATE_VDEC0(CLK_VDEC_VDEC_ENG, "vdec_vdec_eng", "vdec_sel", 8),
	/* VDEC1 */
	GATE_VDEC1(CLK_VDEC_LAT, "vdec_lat", "vdec_sel", 0),
	GATE_VDEC1(CLK_VDEC_LAT_ACTIVE, "vdec_lat_active", "vdec_sel", 4),
	GATE_VDEC1(CLK_VDEC_LAT_ENG, "vdec_lat_eng", "vdec_sel", 8),
	/* VDEC2 */
	GATE_VDEC2(CLK_VDEC_LARB1, "vdec_larb1", "vdec_sel", 0),
};

static const struct mtk_gate vdec_soc_clks[] = {
	/* VDEC_SOC0 */
	GATE_VDEC0(CLK_VDEC_SOC_VDEC, "vdec_soc_vdec", "vdec_sel", 0),
	GATE_VDEC0(CLK_VDEC_SOC_VDEC_ACTIVE, "vdec_soc_vdec_active", "vdec_sel", 4),
	GATE_VDEC0(CLK_VDEC_SOC_VDEC_ENG, "vdec_soc_vdec_cken_eng", "vdec_sel", 8),
	/* VDEC_SOC1 */
	GATE_VDEC1(CLK_VDEC_SOC_LAT, "vdec_soc_lat", "vdec_sel", 0),
	GATE_VDEC1(CLK_VDEC_SOC_LAT_ACTIVE, "vdec_soc_lat_active", "vdec_sel", 4),
	GATE_VDEC1(CLK_VDEC_SOC_LAT_ENG, "vdec_soc_lat_eng", "vdec_sel", 8),
	/* VDEC_SOC2 */
	GATE_VDEC2(CLK_VDEC_SOC_LARB1, "vdec_soc_larb1", "vdec_sel", 0),
};

static const struct mtk_clk_desc vdec_desc = {
	.clks = vdec_clks,
	.num_clks = ARRAY_SIZE(vdec_clks),
};

static const struct mtk_clk_desc vdec_soc_desc = {
	.clks = vdec_soc_clks,
	.num_clks = ARRAY_SIZE(vdec_soc_clks),
};

static const struct of_device_id of_match_clk_mt6893_vdec[] = {
	{ .compatible = "mediatek,mt6893-vdecsys", .data = &vdec_desc },
	{ .compatible = "mediatek,mt6893-vdecsys-soc", .data = &vdec_soc_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_vdec);

static struct platform_driver clk_mt6893_vdec_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-vdec",
		.of_match_table = of_match_clk_mt6893_vdec,
	},
};
module_platform_driver(clk_mt6893_vdec_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 Video Decoders clocks driver");
MODULE_LICENSE("GPL");
