// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 Yassine Oudjana <y.oudjana@protonmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/platform_device.h>

#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mediatek,mt6735-infracfg.h>

#define INFRA_RST0			0x30
#define INFRA_GLOBALCON_PDN0		0x40
#define INFRA_PDN1			0x44
#define	INFRA_PDN_STA			0x48

static struct mtk_gate_regs infra_cg_regs = {
	.set_ofs = INFRA_GLOBALCON_PDN0,
	.clr_ofs = INFRA_PDN1,
	.sta_ofs = INFRA_PDN_STA,
};

static const struct mtk_gate infracfg_gates[] = {
	GATE_MTK(CLK_DBG, "dbg", "axi_sel", &infra_cg_regs, 0, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_GCE, "gce", "axi_sel", &infra_cg_regs, 1, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_TRBG, "trbg", "axi_sel", &infra_cg_regs, 2, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_CPUM, "cpum", "axi_sel", &infra_cg_regs, 3, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_DEVAPC, "devapc", "axi_sel", &infra_cg_regs, 4, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_AUDIO, "audio", "aud_intbus_sel", &infra_cg_regs, 5, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_GCPU, "gcpu", "axi_sel", &infra_cg_regs, 6, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_L2C_SRAM, "l2csram", "axi_sel", &infra_cg_regs, 7, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_M4U, "m4u", "axi_sel", &infra_cg_regs, 8, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_CLDMA, "cldma", "axi_sel", &infra_cg_regs, 12, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_CONNMCU_BUS, "connmcu_bus", "axi_sel", &infra_cg_regs, 15, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_KP, "kp", "axi_sel", &infra_cg_regs, 16, &mtk_clk_gate_ops_setclr),
	GATE_MTK_FLAGS(CLK_APXGPT, "apxgpt", "axi_sel", &infra_cg_regs, 18, &mtk_clk_gate_ops_setclr, CLK_IS_CRITICAL),
	GATE_MTK(CLK_SEJ, "sej", "axi_sel", &infra_cg_regs, 19, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_CCIF0_AP, "ccif0ap", "axi_sel", &infra_cg_regs, 20, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_CCIF1_AP, "ccif1ap", "axi_sel", &infra_cg_regs, 21, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PMIC_SPI, "pmicspi", "pmicspi_sel", &infra_cg_regs, 22, &mtk_clk_gate_ops_setclr),
	GATE_MTK(CLK_PMIC_WRAP, "pmicwrap", "axi_sel", &infra_cg_regs, 23, &mtk_clk_gate_ops_setclr)
};

static u16 infracfg_rst_ofs[] = { INFRA_RST0 };

static const struct mtk_clk_rst_desc infracfg_resets = {
	.version = MTK_RST_SIMPLE,
	.rst_bank_ofs = infracfg_rst_ofs,
	.rst_bank_nr = ARRAY_SIZE(infracfg_rst_ofs)
};

static const struct mtk_clk_desc infracfg_clks = {
	.clks = infracfg_gates,
	.num_clks = ARRAY_SIZE(infracfg_gates),

	.rst_desc = &infracfg_resets
};

static const struct of_device_id of_match_mt6735_infracfg[] = {
	{ .compatible = "mediatek,mt6735-infracfg", .data = &infracfg_clks },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mt6735_infracfg);

static struct platform_driver clk_mt6735_infracfg = {
	.probe = mtk_clk_simple_probe,
	.remove_new = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt6735-infracfg",
		.of_match_table = of_match_mt6735_infracfg,
	},
};
module_platform_driver(clk_mt6735_infracfg);

MODULE_AUTHOR("Yassine Oudjana <y.oudjana@protonmail.com>");
MODULE_DESCRIPTION("MediaTek MT6735 infracfg clock and reset driver");
MODULE_LICENSE("GPL");
