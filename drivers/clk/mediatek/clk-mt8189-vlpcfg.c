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

#include <dt-bindings/clock/mt8189-clk.h>

static const struct mtk_gate_regs vlpcfg_ao_reg_cg_regs = {
	.set_ofs = 0x800,
	.clr_ofs = 0x800,
	.sta_ofs = 0x800,
};

#define GATE_VLPCFG_AO_REG(_id, _name, _parent, _shift) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vlpcfg_ao_reg_cg_regs,		\
		.shift = _shift,			\
		.ops = &mtk_clk_gate_ops_no_setclr,	\
	}

static const struct mtk_gate vlpcfg_ao_reg_clks[] = {
	GATE_VLPCFG_AO_REG(EN, "en", "clk26m", 8),
};

static const struct mtk_clk_desc vlpcfg_ao_reg_mcd = {
	.clks = vlpcfg_ao_reg_clks,
	.num_clks = CLK_VLPCFG_AO_REG_NR_CLK,
};

static const struct mtk_gate_regs vlpcfg_reg_cg_regs = {
	.set_ofs = 0x4,
	.clr_ofs = 0x4,
	.sta_ofs = 0x4,
};

#define GATE_VLPCFG_REG_FLAGS(_id, _name, _parent, _shift, _flags) {	\
		.id = _id,				\
		.name = _name,				\
		.parent_name = _parent,			\
		.regs = &vlpcfg_reg_cg_regs,		\
		.shift = _shift,			\
		.flags = _flags,			\
		.ops = &mtk_clk_gate_ops_no_setclr_inv,	\
	}

#define GATE_VLPCFG_REG(_id, _name, _parent, _shift)		\
	GATE_VLPCFG_REG_FLAGS(_id, _name, _parent, _shift, 0)

static const struct mtk_gate vlpcfg_reg_clks[] = {
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SCP, "vlpcfg_scp_ck",
			      "vlp_scp_sel", 28, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_RG_R_APXGPT_26M,
			      "vlpcfg_r_apxgpt_26m_ck",
			      "clk26m", 24, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_DPMSRCK_TEST,
			      "vlpcfg_dpmsrck_test_ck",
			      "clk26m", 23, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_RG_DPMSRRTC_TEST,
			      "vlpcfg_dpmsrrtc_test_ck",
			      "clk32k", 22, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_DPMSRULP_TEST,
			      "vlpcfg_dpmsrulp_test_ck",
			      "osc_d10", 21, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SPMI_P_MST,
			      "vlpcfg_spmi_p_ck",
			      "vlp_spmi_p_sel", 20, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SPMI_P_MST_32K,
			      "vlpcfg_spmi_p_32k_ck",
			      "clk32k", 18, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_PMIF_SPMI_P_SYS,
			      "vlpcfg_pmif_spmi_p_sys_ck",
			      "vlp_pwrap_ulposc_sel", 13, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_PMIF_SPMI_P_TMR,
			      "vlpcfg_pmif_spmi_p_tmr_ck",
			      "vlp_pwrap_ulposc_sel", 12, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG(CLK_VLPCFG_REG_PMIF_SPMI_M_SYS,
			"vlpcfg_pmif_spmi_m_sys_ck",
			"vlp_pwrap_ulposc_sel", 11),
	GATE_VLPCFG_REG(CLK_VLPCFG_REG_PMIF_SPMI_M_TMR,
			"vlpcfg_pmif_spmi_m_tmr_ck",
			"vlp_pwrap_ulposc_sel", 10),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_DVFSRC,
			      "vlpcfg_dvfsrc_ck",
			      "vlp_dvfsrc_sel", 9, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_PWM_VLP,
			      "vlpcfg_pwm_vlp_ck",
			      "vlp_pwm_vlp_sel", 8, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SRCK,
			      "vlpcfg_srck_ck",
			      "vlp_srck_sel", 7, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SSPM_F26M,
			      "vlpcfg_sspm_f26m_ck",
			      "vlp_sspm_f26m_sel", 4, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SSPM_F32K,
			      "vlpcfg_sspm_f32k_ck",
			      "clk32k", 3, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_SSPM_ULPOSC,
			      "vlpcfg_sspm_ulposc_ck",
			      "vlp_sspm_ulposc_sel", 2, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_VLP_32K_COM,
			      "vlpcfg_vlp_32k_com_ck",
			      "clk32k", 1, CLK_IS_CRITICAL),
	GATE_VLPCFG_REG_FLAGS(CLK_VLPCFG_REG_VLP_26M_COM,
			      "vlpcfg_vlp_26m_com_ck",
			      "clk26m", 0, CLK_IS_CRITICAL),
};

static const struct mtk_clk_desc vlpcfg_reg_mcd = {
	.clks = vlpcfg_reg_clks,
	.num_clks = CLK_VLPCFG_REG_NR_CLK,
};

static const struct of_device_id of_match_clk_mt8189_vlpcfg[] = {
	{
		.compatible = "mediatek,mt8189-vlp-ao-ckgen",
		.data = &vlpcfg_ao_reg_mcd
	}, {
		.compatible = "mediatek,mt8189-vlpcfg-reg-bus",
		.data = &vlpcfg_reg_mcd
	}, {
		/* sentinel */
	}
};

static struct platform_driver clk_mt8189_vlpcfg_drv = {
	.probe = mtk_clk_simple_probe,
	.driver = {
		.name = "clk-mt8189-vlpcfg",
		.of_match_table = of_match_clk_mt8189_vlpcfg,
	},
};

module_platform_driver(clk_mt8189_vlpcfg_drv);
MODULE_LICENSE("GPL");
