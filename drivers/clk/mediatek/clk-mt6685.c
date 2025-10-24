// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 Collabora Ltd.
 *                    AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>
 */
#include <dt-bindings/clock/mediatek,mt6685-clock.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"
#include "clk-mtk-spmi.h"
#include "reset.h"

static const struct mtk_gate_regs spmi_mt6685_sck_top_cg_regs = {
	.set_ofs = 0x1,
	.clr_ofs = 0x2,
	.sta_ofs = 0x0
};

#define GATE_SCKTOP(_id, _name, _parent, _shift)	\
{							\
	.id = _id,					\
	.name = _name,					\
	.parent_name = _parent,				\
	.regs = &spmi_mt6685_sck_top_cg_regs,		\
	.shift = _shift,				\
	.flags = CLK_IGNORE_UNUSED,			\
	.ops = &mtk_clk_gate_ops_setclr,		\
}

static const struct mtk_gate sck_top_clks[] = {
	GATE_SCKTOP(CLK_RTC_SEC_MCLK, "rtc_sec_mclk", "rtc_sec_32k", 0),
	GATE_SCKTOP(CLK_RTC_EOSC32, "rtc_eosc32", "clk26m", 2),
	GATE_SCKTOP(CLK_RTC_SEC_32K, "rtc_sec_32k", "clk26m", 3),
	GATE_SCKTOP(CLK_RTC_MCLK, "rtc_mclk", "rtc_32k", 4),
	GATE_SCKTOP(CLK_RTC_32K, "rtc_32k", "clk26m", 5),
};

static const struct mtk_clk_desc mt6685_sck_top_mcd = {
	.clks = sck_top_clks,
	.num_clks = ARRAY_SIZE(sck_top_clks),
};

static const struct mtk_spmi_clk_desc mt6685_sck_top_mscd = {
	.desc = &mt6685_sck_top_mcd,
	.max_register = 0x10,
};

static const struct of_device_id of_match_clk_mt6685[] = {
	{ .compatible = "mediatek,mt6685-sck-top", .data = &mt6685_sck_top_mscd },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6685);

static struct platform_driver clk_mt6685_spmi_drv = {
	.probe = mtk_spmi_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-spmi-mt6685",
		.of_match_table = of_match_clk_mt6685,
	},
};
module_platform_driver(clk_mt6685_spmi_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6685 SPMI Clock IC clocks driver");
MODULE_LICENSE("GPL");
