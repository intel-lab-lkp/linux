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

static const struct mtk_gate_regs scp_adsp_cg_regs = {
	.set_ofs = 0x180,
	.clr_ofs = 0x180,
	.sta_ofs = 0x180,
};

#define GATE_SCP_ADSP(_id, _name, _parent, _shift)	\
	GATE_MTK(_id, _name, _parent, &scp_adsp_cg_regs, _shift, &mtk_clk_gate_ops_no_setclr)

static const struct mtk_gate scp_adsp_clks[] = {
	GATE_SCP_ADSP(CLK_SCP_ADSP_AUDIODSP, "scp_adsp_audiodsp", "adsp_sel", 0),
};

static const struct mtk_clk_desc scp_adsp_desc = {
	.clks = scp_adsp_clks,
	.num_clks = ARRAY_SIZE(scp_adsp_clks),
};

static const struct of_device_id of_match_clk_mt6893_scp_adsp[] = {
	{ .compatible = "mediatek,mt6893-scp-adsp", .data = &scp_adsp_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_scp_adsp);

static struct platform_driver clk_mt6893_scp_adsp_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6893-scp_adsp",
		.of_match_table = of_match_clk_mt6893_scp_adsp,
	},
};
module_platform_driver(clk_mt6893_scp_adsp_drv);

MODULE_AUTHOR("AngeloGioacchino Del Regno <angelogioacchino.delregno@collabora.com>");
MODULE_DESCRIPTION("MediaTek MT6893 SCP AudioDSP clocks driver");
MODULE_LICENSE("GPL");
