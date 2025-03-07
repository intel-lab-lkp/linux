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

static const struct mtk_gate_regs pext_cg_regs = {
	.set_ofs = 0x18,
	.clr_ofs = 0x1c,
	.sta_ofs = 0x14,
};

#define GATE_PEXT(_id, _name, _parent, _shift) {\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &pext_cg_regs,		\
		.shift = _shift,		\
		.flags = CLK_OPS_PARENT_ENABLE,	\
		.ops = &mtk_clk_gate_ops_setclr,\
	}

#define GATE_PEXT_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate pext_clks[] = {
	GATE_PEXT(CLK_PEXT_PEXTP_MAC_P0_TL, "pext_pm0_tl", "ck_tl_ck", 0),
	GATE_PEXT_V(CLK_PEXT_PEXTP_MAC_P0_TL_PCIE, "pext_pm0_tl_pcie", "pext_pm0_tl"),
	GATE_PEXT(CLK_PEXT_PEXTP_MAC_P0_REF, "pext_pm0_ref", "ck_f26m_ck", 1),
	GATE_PEXT_V(CLK_PEXT_PEXTP_MAC_P0_REF_PCIE, "pext_pm0_ref_pcie", "pext_pm0_ref"),
	GATE_PEXT(CLK_PEXT_PEXTP_PHY_P0_MCU_BUS, "pext_pp0_mcu_bus", "ck_f26m_ck", 6),
	GATE_PEXT_V(CLK_PEXT_PEXTP_PHY_P0_MCU_BUS_PCIE, "pext_pp0_mcu_bus_pcie",
		    "pext_pp0_mcu_bus"),
	GATE_PEXT(CLK_PEXT_PEXTP_PHY_P0_PEXTP_REF, "pext_pp0_pextp_ref", "ck_f26m_ck", 7),
	GATE_PEXT_V(CLK_PEXT_PEXTP_PHY_P0_PEXTP_REF_PCIE, "pext_pp0_pextp_ref_pcie",
		    "pext_pp0_pextp_ref"),
	GATE_PEXT(CLK_PEXT_PEXTP_MAC_P0_AXI_250, "pext_pm0_axi_250", "ck_pexpt0_mem_sub_ck", 12),
	GATE_PEXT_V(CLK_PEXT_PEXTP_MAC_P0_AXI_250_PCIE, "pext_pm0_axi_250_pcie",
		    "pext_pm0_axi_250"),
	GATE_PEXT(CLK_PEXT_PEXTP_MAC_P0_AHB_APB, "pext_pm0_ahb_apb", "ck_pextp0_axi_ck", 13),
	GATE_PEXT_V(CLK_PEXT_PEXTP_MAC_P0_AHB_APB_PCIE, "pext_pm0_ahb_apb_pcie",
		    "pext_pm0_ahb_apb"),
	GATE_PEXT(CLK_PEXT_PEXTP_MAC_P0_PL_P, "pext_pm0_pl_p", "ck_f26m_ck", 14),
	GATE_PEXT_V(CLK_PEXT_PEXTP_MAC_P0_PL_P_PCIE, "pext_pm0_pl_p_pcie", "pext_pm0_pl_p"),
	GATE_PEXT(CLK_PEXT_PEXTP_VLP_AO_P0_LP, "pext_pextp_vlp_ao_p0_lp", "ck_f26m_ck", 19),
	GATE_PEXT_V(CLK_PEXT_PEXTP_VLP_AO_P0_LP_PCIE, "pext_pextp_vlp_ao_p0_lp_pcie",
		    "pext_pextp_vlp_ao_p0_lp"),
};

static const struct mtk_clk_desc pext_mcd = {
	.clks = pext_clks,
	.num_clks = ARRAY_SIZE(pext_clks),
};

static const struct mtk_gate_regs pext1_cg_regs = {
	.set_ofs = 0x18,
	.clr_ofs = 0x1c,
	.sta_ofs = 0x14,
};

#define GATE_PEXT1(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &pext1_cg_regs,			\
		.shift = _shift,			\
		.flags = CLK_OPS_PARENT_ENABLE,		\
		.ops = &mtk_clk_gate_ops_setclr,	\
	}

#define GATE_PEXT1_V(_id, _name, _parent) {	\
		.id = _id,			\
		.name = _name,			\
		.parent_name = _parent,		\
		.regs = &cg_regs_dummy,		\
		.ops = &mtk_clk_dummy_ops,	\
	}

static const struct mtk_gate pext1_clks[] = {
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P1_TL, "pext1_pm1_tl", "ck_tl_p1_ck", 0),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P1_TL_PCIE, "pext1_pm1_tl_pcie", "pext1_pm1_tl"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P1_REF, "pext1_pm1_ref", "ck_f26m_ck", 1),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P1_REF_PCIE, "pext1_pm1_ref_pcie", "pext1_pm1_ref"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P2_TL, "pext1_pm2_tl", "ck_tl_p2_ck", 2),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P2_TL_PCIE, "pext1_pm2_tl_pcie", "pext1_pm2_tl"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P2_REF, "pext1_pm2_ref", "ck_f26m_ck", 3),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P2_REF_PCIE, "pext1_pm2_ref_pcie", "pext1_pm2_ref"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_PHY_P1_MCU_BUS, "pext1_pp1_mcu_bus", "ck_f26m_ck", 8),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_PHY_P1_MCU_BUS_PCIE, "pext1_pp1_mcu_bus_pcie",
		     "pext1_pp1_mcu_bus"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_PHY_P1_PEXTP_REF, "pext1_pp1_pextp_ref", "ck_f26m_ck", 9),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_PHY_P1_PEXTP_REF_PCIE, "pext1_pp1_pextp_ref_pcie",
		     "pext1_pp1_pextp_ref"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_PHY_P2_MCU_BUS, "pext1_pp2_mcu_bus", "ck_f26m_ck", 10),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_PHY_P2_MCU_BUS_PCIE, "pext1_pp2_mcu_bus_pcie",
		     "pext1_pp2_mcu_bus"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_PHY_P2_PEXTP_REF, "pext1_pp2_pextp_ref", "ck_f26m_ck", 11),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_PHY_P2_PEXTP_REF_PCIE, "pext1_pp2_pextp_ref_pcie",
		     "pext1_pp2_pextp_ref"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P1_AXI_250, "pext1_pm1_axi_250",
		   "ck_pextp1_usb_axi_ck", 16),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P1_AXI_250_PCIE, "pext1_pm1_axi_250_pcie",
		     "pext1_pm1_axi_250"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P1_AHB_APB, "pext1_pm1_ahb_apb",
		   "ck_pextp1_usb_mem_sub_ck", 17),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P1_AHB_APB_PCIE, "pext1_pm1_ahb_apb_pcie",
		     "pext1_pm1_ahb_apb"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P1_PL_P, "pext1_pm1_pl_p", "ck_f26m_ck", 18),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P1_PL_P_PCIE, "pext1_pm1_pl_p_pcie", "pext1_pm1_pl_p"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P2_AXI_250, "pext1_pm2_axi_250",
		   "ck_pextp1_usb_axi_ck", 19),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P2_AXI_250_PCIE, "pext1_pm2_axi_250_pcie",
		     "pext1_pm2_axi_250"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P2_AHB_APB, "pext1_pm2_ahb_apb",
		   "ck_pextp1_usb_mem_sub_ck", 20),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P2_AHB_APB_PCIE, "pext1_pm2_ahb_apb_pcie",
		     "pext1_pm2_ahb_apb"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_MAC_P2_PL_P, "pext1_pm2_pl_p", "ck_f26m_ck", 21),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_MAC_P2_PL_P_PCIE, "pext1_pm2_pl_p_pcie", "pext1_pm2_pl_p"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_VLP_AO_P1_LP, "pext1_pextp_vlp_ao_p1_lp", "ck_f26m_ck", 26),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_VLP_AO_P1_LP_PCIE, "pext1_pextp_vlp_ao_p1_lp_pcie",
		     "pext1_pextp_vlp_ao_p1_lp"),
	GATE_PEXT1(CLK_PEXT1_PEXTP_VLP_AO_P2_LP, "pext1_pextp_vlp_ao_p2_lp", "ck_f26m_ck", 27),
	GATE_PEXT1_V(CLK_PEXT1_PEXTP_VLP_AO_P2_LP_PCIE, "pext1_pextp_vlp_ao_p2_lp_pcie",
		     "pext1_pextp_vlp_ao_p2_lp"),
};

static const struct mtk_clk_desc pext1_mcd = {
	.clks = pext1_clks,
	.num_clks = ARRAY_SIZE(pext1_clks),
};

static const struct of_device_id of_match_clk_mt8196_pextp[] = {
	{ .compatible = "mediatek,mt8196-pextp0cfg_ao", .data = &pext_mcd, },
	{ .compatible = "mediatek,mt8196-pextp1cfg_ao", .data = &pext1_mcd, },
	{ /* sentinel */ }
};

static struct platform_driver clk_mt8196_pextp_drv = {
	.probe = mtk_clk_simple_probe,
	.remove = mtk_clk_simple_remove,
	.driver = {
		.name = "clk-mt8196-pextp",
		.of_match_table = of_match_clk_mt8196_pextp,
	},
};

module_platform_driver(clk_mt8196_pextp_drv);
MODULE_LICENSE("GPL");
