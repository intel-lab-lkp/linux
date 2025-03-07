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

static const struct mtk_gate_regs ufsao0_cg_regs = {
	.set_ofs = 0x108,
	.clr_ofs = 0x10c,
	.sta_ofs = 0x104,
};

static const struct mtk_gate_regs ufsao1_cg_regs = {
	.set_ofs = 0x8,
	.clr_ofs = 0xc,
	.sta_ofs = 0x4,
};

#define GATE_UFSAO0(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ufsao0_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_UFSAO0_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

#define GATE_UFSAO1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &ufsao1_cg_regs,		\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_UFSAO1_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate ufsao_clks[] = {
	/* UFSAO0 */
	GATE_UFSAO0(CLK_UFSAO_UFSHCI_UFS, "ufsao_ufshci_ufs", "ck_ck", 0),
	GATE_UFSAO0_V(CLK_UFSAO_UFSHCI_UFS_UFS, "ufsao_ufshci_ufs_ufs", "ufsao_ufshci_ufs"),
	GATE_UFSAO0(CLK_UFSAO_UFSHCI_AES, "ufsao_ufshci_aes", "ck_aes_ufsfde_ck", 1),
	GATE_UFSAO0_V(CLK_UFSAO_UFSHCI_AES_UFS, "ufsao_ufshci_aes_ufs", "ufsao_ufshci_aes"),
	/* UFSAO1 */
	GATE_UFSAO1(CLK_UFSAO_UNIPRO_TX_SYM, "ufsao_unipro_tx_sym", "ck_f26m_ck", 0),
	GATE_UFSAO1_V(CLK_UFSAO_UNIPRO_TX_SYM_UFS, "ufsao_unipro_tx_sym_ufs",
		      "ufsao_unipro_tx_sym"),
	GATE_UFSAO1(CLK_UFSAO_UNIPRO_RX_SYM0, "ufsao_unipro_rx_sym0", "ck_f26m_ck", 1),
	GATE_UFSAO1_V(CLK_UFSAO_UNIPRO_RX_SYM0_UFS, "ufsao_unipro_rx_sym0_ufs",
		      "ufsao_unipro_rx_sym0"),
	GATE_UFSAO1(CLK_UFSAO_UNIPRO_RX_SYM1, "ufsao_unipro_rx_sym1", "ck_f26m_ck", 2),
	GATE_UFSAO1_V(CLK_UFSAO_UNIPRO_RX_SYM1_UFS, "ufsao_unipro_rx_sym1_ufs",
		      "ufsao_unipro_rx_sym1"),
	GATE_UFSAO1(CLK_UFSAO_UNIPRO_SYS, "ufsao_unipro_sys", "ck_ck", 3),
	GATE_UFSAO1_V(CLK_UFSAO_UNIPRO_SYS_UFS, "ufsao_unipro_sys_ufs", "ufsao_unipro_sys"),
	GATE_UFSAO1(CLK_UFSAO_UNIPRO_SAP, "ufsao_unipro_sap", "ck_f26m_ck", 4),
	GATE_UFSAO1_V(CLK_UFSAO_UNIPRO_SAP_UFS, "ufsao_unipro_sap_ufs", "ufsao_unipro_sap"),
	GATE_UFSAO1(CLK_UFSAO_PHY_SAP, "ufsao_phy_sap", "ck_f26m_ck", 8),
	GATE_UFSAO1_V(CLK_UFSAO_PHY_SAP_UFS, "ufsao_phy_sap_ufs", "ufsao_phy_sap"),
};

static const struct mtk_clk_desc ufsao_mcd = {
	.clks = ufsao_clks,
	.num_clks = ARRAY_SIZE(ufsao_clks),
};

static const struct of_device_id of_match_clk_mt8196_ufs_ao[] = {
	{ .compatible = "mediatek,mt8196-ufscfg_ao", .data = &ufsao_mcd, },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_ufs_ao_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-ufs_ao",
		.of_match_table = of_match_clk_mt8196_ufs_ao,
	},
};

module_platform_driver(clk_mt8196_ufs_ao_drv);
MODULE_LICENSE("GPL");
