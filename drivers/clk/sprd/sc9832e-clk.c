// SPDX-License-Identifier: GPL-2.0-only
/*
 * Unisoc SC9832E clock driver
 *
 * Copyright (C) 2015 Spreadtrum, Inc.
 * Copyright (C) 2026 Nadi Ke <kenadicanady@gmail.com>
 * Author: Nadi Ke <kenadicanady@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/mod_devicetable.h>

#include <dt-bindings/clock/sprd,sc9832e-clk.h>

#include "common.h"
#include "composite.h"
#include "div.h"
#include "gate.h"
#include "mux.h"
#include "pll.h"

#define CLK_PMU_GATE_NUM		(CLK_GPLL_GATE + 1)
#define CLK_PLL_NUM				(CLK_ISPPLL_468M + 1)
#define CLK_MPLL_NUM			(CLK_MPLL_50M + 1)
#define CLK_DPLL_NUM			(CLK_DPLL_40M + 1)
#define CLK_RPLL_NUM			(CLK_RPLL_26M + 1)
#define CLK_AP_AHB_GATE_NUM		(CLK_SDIO1_32K_EB + 1)
#define CLK_AON_APB_GATE_NUM	(CLK_CA7_DAP_EB + 1)
#define CLK_AP_CLK_NUM			(CLK_DSI_LANEBYTE + 1)
#define CLK_AON_CLK_NUM			(CLK_MM_ISP + 1)
#define CLK_AP_APB_GATE_NUM		(CLK_INTC3_EB + 1)

static SPRD_PLL_SC_GATE_CLK_FW_NAME(isppll_gate, "isppll-gate", "ext-26m",
				    0x88, 0x1000, BIT(0), 0, 0, 240);
static SPRD_PLL_SC_GATE_CLK_FW_NAME(mpll_gate, "mpll-gate", "ext-26m",
				    0x94, 0x1000, BIT(0), 0, 0, 240);
static SPRD_PLL_SC_GATE_CLK_FW_NAME(dpll_gate, "dpll-gate", "ext-26m",
				    0x98, 0x1000, BIT(0), 0, 0, 240);
static SPRD_PLL_SC_GATE_CLK_FW_NAME(lpll_gate, "lpll-gate", "ext-26m",
				    0x9c, 0x1000, BIT(0), 0, 0, 240);
static SPRD_PLL_SC_GATE_CLK_FW_NAME(gpll_gate, "gpll-gate", "ext-26m",
				    0xa8, 0x1000, BIT(0), 0, 0, 240);

static struct sprd_clk_common *sc9832e_pmu_gate_clks[] = {
	&isppll_gate.common,
	&mpll_gate.common,
	&dpll_gate.common,
	&lpll_gate.common,
	&gpll_gate.common,
};

static struct clk_hw_onecell_data sc9832e_pmu_gate_hws = {
	.hws	= {
		[CLK_ISPPLL_GATE]	= &isppll_gate.common.hw,
		[CLK_MPLL_GATE]		= &mpll_gate.common.hw,
		[CLK_DPLL_GATE]		= &dpll_gate.common.hw,
		[CLK_LPLL_GATE]		= &lpll_gate.common.hw,
		[CLK_GPLL_GATE]		= &gpll_gate.common.hw,
	},
	.num	= CLK_PMU_GATE_NUM,
};

static const struct sprd_clk_desc sc9832e_pmu_gate_desc = {
	.clk_clks	= sc9832e_pmu_gate_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_pmu_gate_clks),
	.hw_clks        = &sc9832e_pmu_gate_hws,
};

static const u64 itable[5] = {4, 1000000000, 1200000000,
			      1400000000, 1600000000};

static const struct clk_bit_field f_pll_layout_a[PLL_FACT_MAX] = {
	{ .shift = 0,	.width = 0 },	/* lock_done */
	{ .shift = 0,	.width = 1 },	/* div_s */
	{ .shift = 1,	.width = 1 },	/* mod_en */
	{ .shift = 2,	.width = 1 },	/* sdm_en */
	{ .shift = 0,	.width = 0 },	/* refin */
	{ .shift = 6,	.width = 2 },	/* ibias */
	{ .shift = 8,	.width = 11 },	/* n */
	{ .shift = 55,	.width = 7 },	/* nint */
	{ .shift = 32,	.width = 23},	/* kint */
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};

static const struct clk_bit_field f_pll_layout_b[PLL_FACT_MAX] = {
	{ .shift = 0,	.width = 0 },	/* lock_done */
	{ .shift = 1,	.width = 1 },	/* div_s */
	{ .shift = 2,	.width = 1 },	/* mod_en */
	{ .shift = 3,	.width = 1 },	/* sdm_en */
	{ .shift = 0,	.width = 1 },	/* refin */
	{ .shift = 7,	.width = 2 },	/* ibias */
	{ .shift = 9,	.width = 11 },	/* n */
	{ .shift = 55,	.width = 7 },	/* nint */
	{ .shift = 32,	.width = 23},	/* kint */
	{ .shift = 0,	.width = 0 },	/* prediv */
	{ .shift = 0,	.width = 0 },	/* postdiv */
};

static const struct clk_bit_field f_pll_layout_c[PLL_FACT_MAX] = {
	{ .shift = 0,	.width = 0 },	/* lock_done */
	{ .shift = 9,	.width = 1 },	/* div_s */
	{ .shift = 10,	.width = 1 },	/* mod_en */
	{ .shift = 11,	.width = 1 },	/* sdm_en */
	{ .shift = 0,	.width = 0 },	/* refin */
	{ .shift = 15,	.width = 2 },	/* ibias */
	{ .shift = 17,	.width = 11 },	/* n */
	{ .shift = 55,	.width = 7 },	/* nint */
	{ .shift = 32,	.width = 23},	/* kint */
	{ .shift = 0,	.width = 0 },	/* prediv */
	{ .shift = 0,	.width = 1 },	/* postdiv */
};

static const struct clk_bit_field f_pll_layout_d[PLL_FACT_MAX] = {
	{ .shift = 0,	.width = 0 },	/* lock_done */
	{ .shift = 3,	.width = 1 },	/* div_s */
	{ .shift = 4,	.width = 1 },	/* mod_en */
	{ .shift = 5,	.width = 1 },	/* sdm_en */
	{ .shift = 1,	.width = 2 },	/* refin */
	{ .shift = 9,	.width = 2 },	/* ibias */
	{ .shift = 11,	.width = 11 },	/* n */
	{ .shift = 55,	.width = 7 },	/* nint */
	{ .shift = 32,	.width = 23},	/* kint */
	{ .shift = 0,	.width = 0 },	/* prediv */
	{ .shift = 0,	.width = 0 },	/* postdiv */
};

static SPRD_PLL_FW_NAME(twpll, "twpll", "ext-26m", 0x0c, 3, itable,
			f_pll_layout_a, 240, 1000, 1000, 0, 0);
static CLK_FIXED_FACTOR_HW(twpll_768m, "twpll-768m", &twpll.common.hw, 2, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_384m, "twpll-384m", &twpll.common.hw, 4, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_192m, "twpll-192m", &twpll.common.hw, 8, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_96m, "twpll-96m", &twpll.common.hw, 16, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_48m, "twpll-48m", &twpll.common.hw, 32, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_24m, "twpll-24m", &twpll.common.hw, 64, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_12m, "twpll-12m", &twpll.common.hw, 128, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_512m, "twpll-512m", &twpll.common.hw, 3, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_256m, "twpll-256m", &twpll.common.hw, 6, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_128m, "twpll-128m", &twpll.common.hw, 12, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_64m, "twpll-64m", &twpll.common.hw, 24, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_307m2, "twpll-307m2", &twpll.common.hw, 5, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_219m4, "twpll-219m4", &twpll.common.hw, 7, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_170m6, "twpll-170m6", &twpll.common.hw, 9, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_153m6, "twpll-153m6", &twpll.common.hw, 10, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_76m8, "twpll-76m8", &twpll.common.hw, 20, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_51m2, "twpll-51m2", &twpll.common.hw, 30, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_38m4, "twpll-38m4", &twpll.common.hw, 40, 1, 0);
static CLK_FIXED_FACTOR_HW(twpll_19m2, "twpll-19m2", &twpll.common.hw, 80, 1, 0);

static SPRD_PLL_HW(lpll, "lpll", &lpll_gate.common.hw, 0x1c, 3, itable,
		   f_pll_layout_a, 240, 1000, 1000, 0, 0);
static CLK_FIXED_FACTOR_HW(lpll_409m6, "lpll-409m6", &lpll.common.hw, 3, 1, 0);
static CLK_FIXED_FACTOR_HW(lpll_245m76, "lpll-245m76", &lpll.common.hw, 5, 1, 0);

static SPRD_PLL_HW(gpll, "gpll", &gpll_gate.common.hw, 0x2c, 3, itable,
		   f_pll_layout_b, 240, 1000, 1000, 0, 0);

static SPRD_PLL_HW(isppll, "isppll", &isppll_gate.common.hw, 0x3c, 3, itable,
		   f_pll_layout_b, 240, 1000, 1000, 0, 0);
static CLK_FIXED_FACTOR_HW(isppll_468m, "isppll-468m", &isppll.common.hw, 2, 1, 0);

static struct sprd_clk_common *sc9832e_pll_clks[] = {
	&twpll.common,
	&lpll.common,
	&gpll.common,
	&isppll.common,
};

static struct clk_hw_onecell_data sc9832e_pll_hws = {
	.hws	= {
		[CLK_TWPLL]		= &twpll.common.hw,
		[CLK_TWPLL_768M]	= &twpll_768m.hw,
		[CLK_TWPLL_384M]	= &twpll_384m.hw,
		[CLK_TWPLL_192M]	= &twpll_192m.hw,
		[CLK_TWPLL_96M]		= &twpll_96m.hw,
		[CLK_TWPLL_48M]		= &twpll_48m.hw,
		[CLK_TWPLL_24M]		= &twpll_24m.hw,
		[CLK_TWPLL_12M]		= &twpll_12m.hw,
		[CLK_TWPLL_512M]	= &twpll_512m.hw,
		[CLK_TWPLL_256M]	= &twpll_256m.hw,
		[CLK_TWPLL_128M]	= &twpll_128m.hw,
		[CLK_TWPLL_64M]		= &twpll_64m.hw,
		[CLK_TWPLL_307M2]	= &twpll_307m2.hw,
		[CLK_TWPLL_219M4]	= &twpll_219m4.hw,
		[CLK_TWPLL_170M6]	= &twpll_170m6.hw,
		[CLK_TWPLL_153M6]	= &twpll_153m6.hw,
		[CLK_TWPLL_76M8]	= &twpll_76m8.hw,
		[CLK_TWPLL_51M2]	= &twpll_51m2.hw,
		[CLK_TWPLL_38M4]	= &twpll_38m4.hw,
		[CLK_TWPLL_19M2]	= &twpll_19m2.hw,
		[CLK_LPLL]		= &lpll.common.hw,
		[CLK_LPLL_409M6]	= &lpll_409m6.hw,
		[CLK_LPLL_245M76]	= &lpll_245m76.hw,
		[CLK_GPLL]		= &gpll.common.hw,
		[CLK_ISPPLL]		= &isppll.common.hw,
		[CLK_ISPPLL_468M]	= &isppll_468m.hw,
	},
	.num	= CLK_PLL_NUM,
};

static const struct sprd_clk_desc sc9832e_pll_desc = {
	.clk_clks	= sc9832e_pll_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_pll_clks),
	.hw_clks        = &sc9832e_pll_hws,
};

static SPRD_PLL_HW(mpll, "mpll", &mpll_gate.common.hw, 0x0, 3, itable,
		   f_pll_layout_a, 240, 1000, 1000, 0, 0);
static CLK_FIXED_FACTOR_HW(mpll_50m, "mpll-50m", &mpll.common.hw, 18, 1, 0);

static struct sprd_clk_common *sc9832e_mpll_clks[] = {
	&mpll.common,
};

static struct clk_hw_onecell_data sc9832e_mpll_hws = {
	.hws	= {
		[CLK_MPLL]	= &mpll.common.hw,
		[CLK_MPLL_50M]	= &mpll_50m.hw,
	},
	.num	= CLK_MPLL_NUM,
};

static const struct sprd_clk_desc sc9832e_mpll_desc = {
	.clk_clks	= sc9832e_mpll_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_mpll_clks),
	.hw_clks        = &sc9832e_mpll_hws,
};

static SPRD_PLL_HW(dpll, "dpll", &dpll_gate.common.hw, 0x0, 3, itable,
		   f_pll_layout_c, 240, 1000, 1000, 0, 0);
static CLK_FIXED_FACTOR_HW(dpll_40m, "dpll-40m", &dpll.common.hw, 32, 1, 0);

static struct sprd_clk_common *sc9832e_dpll_clks[] = {
	&dpll.common,
};

static struct clk_hw_onecell_data sc9832e_dpll_hws = {
	.hws	= {
		[CLK_DPLL]	= &dpll.common.hw,
		[CLK_DPLL_40M]	= &dpll_40m.hw,
	},
	.num	= CLK_DPLL_NUM,
};

static const struct sprd_clk_desc sc9832e_dpll_desc = {
	.clk_clks	= sc9832e_dpll_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_dpll_clks),
	.hw_clks        = &sc9832e_dpll_hws,
};

static SPRD_SC_GATE_CLK_FW_NAME(audio_gate, "audio-gate", "ext-26m",
				0x08, 0x1000, BIT(8), 0, 0);
static SPRD_PLL_FW_NAME(rpll, "rpll", "ext-26m", 0x14,
			3, itable, f_pll_layout_d, 240, 1000, 1000, 0, 0);

static CLK_FIXED_FACTOR_HW(rpll_390m, "rpll-390m", &rpll.common.hw, 2, 1, 0);
static CLK_FIXED_FACTOR_HW(rpll_260m, "rpll-260m", &rpll.common.hw, 3, 1, 0);
static CLK_FIXED_FACTOR_HW(rpll_195m, "rpll-195m", &rpll.common.hw, 4, 1, 0);
static CLK_FIXED_FACTOR_HW(rpll_26m, "rpll-26m", &rpll.common.hw, 30, 1, 0);

static struct sprd_clk_common *sc9832e_rpll_clks[] = {
	&audio_gate.common,
	&rpll.common,
};

static struct clk_hw_onecell_data sc9832e_rpll_hws = {
	.hws	= {
		[CLK_AUDIO_GATE]	= &audio_gate.common.hw,
		[CLK_RPLL]		= &rpll.common.hw,
		[CLK_RPLL_390M]		= &rpll_390m.hw,
		[CLK_RPLL_260M]		= &rpll_260m.hw,
		[CLK_RPLL_195M]		= &rpll_195m.hw,
		[CLK_RPLL_26M]		= &rpll_26m.hw,
	},
	.num	= CLK_RPLL_NUM,
};

static const struct sprd_clk_desc sc9832e_rpll_desc = {
	.clk_clks	= sc9832e_rpll_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_rpll_clks),
	.hw_clks        = &sc9832e_rpll_hws,
};

static SPRD_SC_GATE_CLK_FW_NAME(dsi_eb, "dsi-eb", "ap-axi", 0x0, 0x1000, BIT(0), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(dispc_eb, "dispc-eb", "ap-axi", 0x0, 0x1000, BIT(1), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(vsp_eb, "vsp-eb", "ap-axi", 0x0, 0x1000, BIT(2), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(gsp_eb, "gsp-eb", "ap-axi", 0x0, 0x1000, BIT(3), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(otg_eb, "otg-eb", "ap-axi", 0x0, 0x1000, BIT(4), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(dma_pub_eb, "dma-pub-eb", "ap-axi", 0x0, 0x1000, BIT(5), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ce_pub_eb, "ce-pub-eb", "ap-axi", 0x0, 0x1000, BIT(6), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ahb_ckg_eb, "ahb-ckg-eb", "ap-axi", 0x0, 0x1000, BIT(7), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(sdio0_eb, "sdio0-eb", "ap-axi", 0x0, 0x1000, BIT(8), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(sdio1_eb, "sdio1-eb", "ap-axi", 0x0, 0x1000, BIT(9), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(nandc_eb, "nandc-eb", "ap-axi", 0x0, 0x1000, BIT(10), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(emmc_eb, "emmc-eb", "ap-axi", 0x0, 0x1000, BIT(11), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(spinlock_eb, "spinlock-eb", "ap-axi", 0x0, 0x1000, BIT(12), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ce_efuse_eb, "ce-efuse-eb", "ap-axi", 0x0, 0x1000, BIT(13), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(emmc_32k_eb, "emmc-32k-eb", "ap-axi", 0x0, 0x1000, BIT(14), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(sdio0_32k_eb, "sdio0-32k-eb", "ap-axi", 0x0, 0x1000, BIT(15), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(sdio1_32k_eb, "sdio1-32k-eb", "ap-axi", 0x0, 0x1000, BIT(16), 0, 0);

static struct sprd_clk_common *sc9832e_apahb_gate_clks[] = {
	&dsi_eb.common,
	&dispc_eb.common,
	&vsp_eb.common,
	&gsp_eb.common,
	&otg_eb.common,
	&dma_pub_eb.common,
	&ce_pub_eb.common,
	&ahb_ckg_eb.common,
	&sdio0_eb.common,
	&sdio1_eb.common,
	&nandc_eb.common,
	&emmc_eb.common,
	&spinlock_eb.common,
	&ce_efuse_eb.common,
	&emmc_32k_eb.common,
	&sdio0_32k_eb.common,
	&sdio1_32k_eb.common,
};

static struct clk_hw_onecell_data sc9832e_apahb_gate_hws = {
	.hws	= {
		[CLK_DSI_EB]		= &dsi_eb.common.hw,
		[CLK_DISPC_EB]		= &dispc_eb.common.hw,
		[CLK_VSP_EB]		= &vsp_eb.common.hw,
		[CLK_GSP_EB]		= &gsp_eb.common.hw,
		[CLK_OTG_EB]		= &otg_eb.common.hw,
		[CLK_DMA_PUB_EB]	= &dma_pub_eb.common.hw,
		[CLK_CE_PUB_EB]		= &ce_pub_eb.common.hw,
		[CLK_AHB_CKG_EB]	= &ahb_ckg_eb.common.hw,
		[CLK_SDIO0_EB]		= &sdio0_eb.common.hw,
		[CLK_SDIO1_EB]		= &sdio1_eb.common.hw,
		[CLK_NANDC_EB]		= &nandc_eb.common.hw,
		[CLK_EMMC_EB]		= &emmc_eb.common.hw,
		[CLK_SPINLOCK_EB]	= &spinlock_eb.common.hw,
		[CLK_CE_EFUSE_EB]	= &ce_efuse_eb.common.hw,
		[CLK_EMMC_32K_EB]	= &emmc_32k_eb.common.hw,
		[CLK_SDIO0_32K_EB]	= &sdio0_32k_eb.common.hw,
		[CLK_SDIO1_32K_EB]	= &sdio1_32k_eb.common.hw,
	},
	.num	= CLK_AP_AHB_GATE_NUM,
};

static const struct sprd_clk_desc sc9832e_apahb_gate_desc = {
	.clk_clks	= sc9832e_apahb_gate_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_apahb_gate_clks),
	.hw_clks	= &sc9832e_apahb_gate_hws,
};

static SPRD_SC_GATE_CLK_FW_NAME(adc_eb, "adc-eb", "clk_aon_apb", 0x0, 0x1000, BIT(0), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(fm_eb, "fm-eb", "clk_aon_apb", 0x0, 0x1000, BIT(1), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(tpc_eb, "tpc-eb", "clk_aon_apb", 0x0, 0x1000, BIT(2), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(gpio_eb, "gpio-eb", "clk_aon_apb", 0x0, 0x1000, BIT(3), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(pwm0_eb, "pwm0-eb", "clk_aon_apb", 0x0, 0x1000, BIT(4), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(pwm1_eb, "pwm1-eb", "clk_aon_apb", 0x0, 0x1000, BIT(5), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(pwm2_eb, "pwm2-eb", "clk_aon_apb", 0x0, 0x1000, BIT(6), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(pwm3_eb, "pwm3-eb", "clk_aon_apb", 0x0, 0x1000, BIT(7), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(kpd_eb, "kpd-eb", "clk_aon_apb", 0x0, 0x1000, BIT(8), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(aon_syst_eb, "aon-syst-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(9), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ap_syst_eb, "ap-syst-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(10), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(aon_tmr_eb, "aon-tmr-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(11), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ap_tmr0_eb, "ap-tmr0-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(12), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(efuse_eb, "efuse-eb", "clk_aon_apb", 0x0, 0x1000, BIT(13), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(eic_eb, "eic-eb", "clk_aon_apb", 0x0, 0x1000, BIT(14), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(intc_eb, "intc-eb", "clk_aon_apb", 0x0, 0x1000, BIT(15), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(adi_eb, "adi-eb", "clk_aon_apb", 0x0, 0x1000, BIT(16), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(audif_eb, "audif-eb", "clk_aon_apb", 0x0, 0x1000, BIT(17), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(aud_eb, "aud-eb", "clk_aon_apb", 0x0, 0x1000, BIT(18), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(vbc_eb, "vbc-eb", "clk_aon_apb", 0x0, 0x1000, BIT(19), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(pin_eb, "pin-eb", "clk_aon_apb", 0x0, 0x1000, BIT(20), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ipi_eb, "ipi-eb", "clk_aon_apb", 0x0, 0x1000, BIT(21), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(splk_eb, "splk-eb", "clk_aon_apb", 0x0, 0x1000, BIT(22), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ap_wdg_eb, "ap-wdg-eb", "clk_aon_apb", 0x0, 0x1000, BIT(24), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(mm_eb, "mm-eb", "clk_aon_apb", 0x0, 0x1000, BIT(25), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(aon_apb_ckg_eb, "aon-apb-ckg-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(26), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(gpu_eb, "gpu-eb", "clk_aon_apb", 0x0, 0x1000, BIT(27), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ca7_ts0_eb, "ca7-ts0-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(28), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(ca7_dap_eb, "ca7-dap-eb", "clk_aon_apb",
				0x0, 0x1000, BIT(29), 0, 0);

static struct sprd_clk_common *sc9832e_aonapb_gate_clks[] = {
	&adc_eb.common,
	&fm_eb.common,
	&tpc_eb.common,
	&gpio_eb.common,
	&pwm0_eb.common,
	&pwm1_eb.common,
	&pwm2_eb.common,
	&pwm3_eb.common,
	&kpd_eb.common,
	&aon_syst_eb.common,
	&ap_syst_eb.common,
	&aon_tmr_eb.common,
	&ap_tmr0_eb.common,
	&efuse_eb.common,
	&eic_eb.common,
	&intc_eb.common,
	&adi_eb.common,
	&audif_eb.common,
	&aud_eb.common,
	&vbc_eb.common,
	&pin_eb.common,
	&ipi_eb.common,
	&splk_eb.common,
	&ap_wdg_eb.common,
	&mm_eb.common,
	&aon_apb_ckg_eb.common,
	&gpu_eb.common,
	&ca7_ts0_eb.common,
	&ca7_dap_eb.common,
};

static struct clk_hw_onecell_data sc9832e_aonapb_gate_hws = {
	.hws	= {
		[CLK_ADC_EB]		= &adc_eb.common.hw,
		[CLK_FM_EB]		= &fm_eb.common.hw,
		[CLK_TPC_EB]		= &tpc_eb.common.hw,
		[CLK_GPIO_EB]		= &gpio_eb.common.hw,
		[CLK_PWM0_EB]		= &pwm0_eb.common.hw,
		[CLK_PWM1_EB]		= &pwm1_eb.common.hw,
		[CLK_PWM2_EB]		= &pwm2_eb.common.hw,
		[CLK_PWM3_EB]		= &pwm3_eb.common.hw,
		[CLK_KPD_EB]		= &kpd_eb.common.hw,
		[CLK_AON_SYST_EB]	= &aon_syst_eb.common.hw,
		[CLK_AP_SYST_EB]	= &ap_syst_eb.common.hw,
		[CLK_AON_TMR_EB]	= &aon_tmr_eb.common.hw,
		[CLK_AP_TMR0_EB]	= &ap_tmr0_eb.common.hw,
		[CLK_EFUSE_EB]		= &efuse_eb.common.hw,
		[CLK_EIC_EB]		= &eic_eb.common.hw,
		[CLK_INTC_EB]		= &intc_eb.common.hw,
		[CLK_ADI_EB]		= &adi_eb.common.hw,
		[CLK_AUDIF_EB]		= &audif_eb.common.hw,
		[CLK_AUD_EB]		= &aud_eb.common.hw,
		[CLK_VBC_EB]		= &vbc_eb.common.hw,
		[CLK_PIN_EB]		= &pin_eb.common.hw,
		[CLK_IPI_EB]		= &ipi_eb.common.hw,
		[CLK_SPLK_EB]		= &splk_eb.common.hw,
		[CLK_AP_WDG_EB]		= &ap_wdg_eb.common.hw,
		[CLK_MM_EB]		= &mm_eb.common.hw,
		[CLK_AON_APB_CKG_EB]	= &aon_apb_ckg_eb.common.hw,
		[CLK_GPU_EB]		= &gpu_eb.common.hw,
		[CLK_CA7_TS0_EB]	= &ca7_ts0_eb.common.hw,
		[CLK_CA7_DAP_EB]	= &ca7_dap_eb.common.hw,
	},
	.num	= CLK_AON_APB_GATE_NUM,
};

static const struct sprd_clk_desc sc9832e_aonapb_gate_desc = {
	.clk_clks	= sc9832e_aonapb_gate_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_aonapb_gate_clks),
	.hw_clks	= &sc9832e_aonapb_gate_hws,
};

#define SC9832E_MUX_FLAG	\
	(CLK_GET_RATE_NOCACHE | CLK_SET_RATE_NO_REPARENT)

static const struct clk_parent_data ap_apb_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_64m.hw  },
	{ .hw = &twpll_96m.hw  },
	{ .hw = &twpll_128m.hw  },
};

static SPRD_MUX_CLK_DATA(ap_apb, "ap-apb", ap_apb_parents, 0x20,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data nandc_ecc_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_256m.hw  },
	{ .hw = &twpll_307m2.hw  },
};

static SPRD_COMP_CLK_DATA(nandc_ecc, "nandc-ecc", nandc_ecc_parents, 0x24,
			  0, 2, 8, 3, 0);

static const struct clk_parent_data otg_ref_parents[] = {
	{ .hw = &twpll_12m.hw  },
	{ .hw = &twpll_24m.hw  },
};

static SPRD_MUX_CLK_DATA(otg_ref_clk, "otg-ref-clk", otg_ref_parents, 0x28,
			 0, 1, SC9832E_MUX_FLAG);

static SPRD_GATE_CLK_HW(otg_utmi, "otg-utmi", &ap_apb.common.hw, 0x2c,
			BIT(16), 0, 0);

static const struct clk_parent_data ap_uart_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_48m.hw  },
	{ .hw = &twpll_51m2.hw  },
	{ .hw = &twpll_96m.hw  },
};

static SPRD_COMP_CLK_DATA(ap_uart1, "ap-uart1", ap_uart_parents, 0x30,
			  0, 2, 8, 3, 0);

static const struct clk_parent_data i2c_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_48m.hw  },
	{ .hw = &twpll_51m2.hw  },
	{ .hw = &twpll_153m6.hw  },
};

static SPRD_COMP_CLK_DATA(ap_i2c0, "ap-i2c0", i2c_parents, 0x34,
			  0, 2, 8, 3, 0);
static SPRD_COMP_CLK_DATA(ap_i2c1, "ap-i2c1", i2c_parents, 0x38,
			  0, 2, 8, 3, 0);
static SPRD_COMP_CLK_DATA(ap_i2c2, "ap-i2c2", i2c_parents, 0x3c,
			  0, 2, 8, 3, 0);
static SPRD_COMP_CLK_DATA(ap_i2c3, "ap-i2c3", i2c_parents, 0x40,
			  0, 2, 8, 3, 0);
static SPRD_COMP_CLK_DATA(ap_i2c4, "ap-i2c4", i2c_parents, 0x44,
			  0, 2, 8, 3, 0);

static const struct clk_parent_data spi_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_153m6.hw  },
	{ .hw = &twpll_192m.hw  },
};

static SPRD_COMP_CLK_DATA(ap_spi0, "ap-spi0", spi_parents, 0x48,
			  0, 2, 8, 3, 0);
static SPRD_COMP_CLK_DATA(ap_spi2, "ap-spi2", spi_parents, 0x4c,
			  0, 2, 8, 3, 0);
static SPRD_COMP_CLK_DATA(ap_hs_spi, "ap-hs-spi", spi_parents, 0x50,
			  0, 2, 8, 3, 0);

static const struct clk_parent_data iis_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_153m6.hw  },
};

static SPRD_COMP_CLK_DATA(ap_iis0, "ap-iis0", iis_parents, 0x54,
			  0, 2, 8, 3, 0);

static const struct clk_parent_data ap_ce_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_96m.hw  },
	{ .hw = &twpll_192m.hw  },
	{ .hw = &twpll_256m.hw  },
};

static SPRD_MUX_CLK_DATA(ap_ce, "ap-ce", ap_ce_parents, 0x58,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data nandc_2x_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_153m6.hw  },
	{ .hw = &twpll_170m6.hw  },
	{ .hw = &rpll_195m.hw  },
	{ .hw = &twpll_219m4.hw  },
	{ .hw = &lpll_245m76.hw  },
	{ .hw = &rpll_260m.hw  },
	{ .hw = &twpll_307m2.hw  },
	{ .hw = &rpll_390m.hw  },
};

static SPRD_COMP_CLK_DATA(nandc_2x, "nandc-2x", nandc_2x_parents, 0x78,
			  0, 4, 8, 4, 0);

static const struct clk_parent_data sdio_parents[] = {
	{ .fw_name = "ext-1m" },
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_307m2.hw  },
	{ .hw = &twpll_384m.hw  },
	{ .hw = &rpll_390m.hw  },
	{ .hw = &lpll_409m6.hw  },
};

static SPRD_MUX_CLK_DATA(sdio0_2x, "sdio0-2x", sdio_parents, 0x80,
			 0, 3, SC9832E_MUX_FLAG);
static SPRD_MUX_CLK_DATA(sdio1_2x, "sdio1-2x", sdio_parents, 0x88,
			 0, 3, SC9832E_MUX_FLAG);
static SPRD_MUX_CLK_DATA(emmc_2x, "emmc-2x", sdio_parents, 0x90,
			 0, 3, SC9832E_MUX_FLAG);

static const struct clk_parent_data vsp_parents[] = {
	{ .hw = &twpll_76m8.hw  },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_256m.hw  },
	{ .hw = &twpll_307m2.hw  },
};

static SPRD_MUX_CLK_DATA(clk_vsp, "vsp-clk", vsp_parents, 0x98,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data gsp_parents[] = {
	{ .hw = &twpll_153m6.hw  },
	{ .hw = &twpll_192m.hw  },
	{ .hw = &twpll_256m.hw  },
	{ .hw = &twpll_384m.hw  },
};

static SPRD_MUX_CLK_DATA(clk_gsp, "gsp-clk", gsp_parents, 0x9c,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data dispc0_parents[] = {
	{ .hw = &twpll_153m6.hw  },
	{ .hw = &twpll_192m.hw  },
	{ .hw = &twpll_256m.hw  },
	{ .hw = &twpll_384m.hw  },
};

static SPRD_MUX_CLK_DATA(clk_dispc0, "dispc0-clk", dispc0_parents, 0xa0,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data dispc0_dpi_parents[] = {
	{ .hw = &twpll_96m.hw  },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_153m6.hw  },
};

static SPRD_COMP_CLK_DATA(clk_dispc0_dpi, "dispc0-dpi-clk", dispc0_dpi_parents, 0xa4,
			  0, 2, 8, 4, 0);

static SPRD_GATE_CLK_HW(dsi_rxesc, "dsi-rxesc", &ap_apb.common.hw, 0xa8,
			BIT(16), 0, 0);
static SPRD_GATE_CLK_HW(dsi_lanebyte, "dsi-lanebyte", &ap_apb.common.hw, 0xac,
			BIT(16), 0, 0);

static struct sprd_clk_common *sc9832e_ap_clks[] = {
	&ap_apb.common,
	&nandc_ecc.common,
	&otg_ref_clk.common,
	&otg_utmi.common,
	&ap_uart1.common,
	&ap_i2c0.common,
	&ap_i2c1.common,
	&ap_i2c2.common,
	&ap_i2c3.common,
	&ap_i2c4.common,
	&ap_spi0.common,
	&ap_spi2.common,
	&ap_hs_spi.common,
	&ap_iis0.common,
	&ap_ce.common,
	&nandc_2x.common,
	&sdio0_2x.common,
	&sdio1_2x.common,
	&emmc_2x.common,
	&clk_vsp.common,
	&clk_gsp.common,
	&clk_dispc0.common,
	&clk_dispc0_dpi.common,
	&dsi_rxesc.common,
	&dsi_lanebyte.common,
};

static struct clk_hw_onecell_data sc9832e_ap_clk_hws = {
	.hws	= {
		[CLK_AP_APB]		= &ap_apb.common.hw,
		[CLK_NANDC_ECC]		= &nandc_ecc.common.hw,
		[CLK_OTG_REF]		= &otg_ref_clk.common.hw,
		[CLK_OTG_UTMI]		= &otg_utmi.common.hw,
		[CLK_UART1]		= &ap_uart1.common.hw,
		[CLK_I2C0]		= &ap_i2c0.common.hw,
		[CLK_I2C1]		= &ap_i2c1.common.hw,
		[CLK_I2C2]		= &ap_i2c2.common.hw,
		[CLK_I2C3]		= &ap_i2c3.common.hw,
		[CLK_I2C4]		= &ap_i2c4.common.hw,
		[CLK_SPI0]		= &ap_spi0.common.hw,
		[CLK_SPI2]		= &ap_spi2.common.hw,
		[CLK_HS_SPI]		= &ap_hs_spi.common.hw,
		[CLK_IIS0]		= &ap_iis0.common.hw,
		[CLK_CE]		= &ap_ce.common.hw,
		[CLK_NANDC_2X]		= &nandc_2x.common.hw,
		[CLK_SDIO0_2X]		= &sdio0_2x.common.hw,
		[CLK_SDIO1_2X]		= &sdio1_2x.common.hw,
		[CLK_EMMC_2X]		= &emmc_2x.common.hw,
		[CLK_VSP]		= &clk_vsp.common.hw,
		[CLK_GSP]		= &clk_gsp.common.hw,
		[CLK_DISPC0]		= &clk_dispc0.common.hw,
		[CLK_DISPC0_DPI]	= &clk_dispc0_dpi.common.hw,
		[CLK_DSI_RXESC]		= &dsi_rxesc.common.hw,
		[CLK_DSI_LANEBYTE]	= &dsi_lanebyte.common.hw,
	},
	.num	= CLK_AP_CLK_NUM,
};

static const struct sprd_clk_desc sc9832e_ap_clk_desc = {
	.clk_clks	= sc9832e_ap_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_ap_clks),
	.hw_clks	= &sc9832e_ap_clk_hws,
};

static const struct clk_parent_data aon_apb_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_76m8.hw  },
	{ .hw = &twpll_96m.hw  },
	{ .hw = &twpll_128m.hw  },
};

static SPRD_COMP_CLK_DATA(aon_apb, "aon-apb", aon_apb_parents, 0x220,
			  0, 2, 8, 2, 0);

static const struct clk_parent_data adi_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_38m4.hw  },
	{ .hw = &twpll_51m2.hw  },
};

static SPRD_MUX_CLK_DATA(adi_clk, "adi-clk", adi_parents, 0x224,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data aux_parents[] = {
	{ .fw_name = "ext-32k" },
	{ .hw = &rpll_26m.hw  },
	{ .fw_name = "ext-26m" },
};

static SPRD_COMP_CLK_DATA(aux0_clk, "aux0-clk", aux_parents, 0x088,
			  0, 2, 16, 4, 0);
static SPRD_COMP_CLK_DATA(aux1_clk, "aux1-clk", aux_parents, 0x088,
			  4, 2, 20, 4, 0);

static const struct clk_parent_data pwm_parents[] = {
	{ .fw_name = "ext-32k" },
	{ .fw_name = "ext-26m" },
	{ .hw = &rpll_26m.hw  },
	{ .hw = &twpll_48m.hw  },
};

static SPRD_MUX_CLK_DATA(pwm0_clk, "pwm0-clk", pwm_parents, 0x238,
			 0, 2, SC9832E_MUX_FLAG);
static SPRD_MUX_CLK_DATA(pwm1_clk, "pwm1-clk", pwm_parents, 0x23c,
			 0, 2, SC9832E_MUX_FLAG);
static SPRD_MUX_CLK_DATA(pwm2_clk, "pwm2-clk", pwm_parents, 0x240,
			 0, 2, SC9832E_MUX_FLAG);
static SPRD_MUX_CLK_DATA(pwm3_clk, "pwm3-clk", pwm_parents, 0x244,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data thm_parents[] = {
	{ .fw_name = "ext-32k" },
	{ .fw_name = "ext-250k" },
};

static SPRD_MUX_CLK_DATA(thm0_clk, "thm0-clk", thm_parents, 0x258,
			 0, 1, SC9832E_MUX_FLAG);
static SPRD_MUX_CLK_DATA(thm1_clk, "thm1-clk", thm_parents, 0x25c,
			 0, 1, SC9832E_MUX_FLAG);

static const struct clk_parent_data audif_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_38m4.hw  },
	{ .hw = &twpll_51m2.hw  },
};

static SPRD_MUX_CLK_DATA(audif_clk, "audif-clk", audif_parents, 0x264,
			 0, 2, SC9832E_MUX_FLAG);

static SPRD_GATE_CLK_HW(aud_iis_da0, "aud-iis-da0", &ap_apb.common.hw, 0x26c,
			BIT(16), 0, 0);
static SPRD_GATE_CLK_HW(aud_iis_ad0, "aud-iis-ad0", &ap_apb.common.hw, 0x270,
			BIT(16), 0, 0);

static const struct clk_parent_data ca53_dap_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_76m8.hw  },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_153m6.hw  },
};

static SPRD_MUX_CLK_DATA(ca53_dap_clk, "ca53-dap-clk", ca53_dap_parents, 0x274,
			 0, 2, SC9832E_MUX_FLAG);

static SPRD_GATE_CLK_HW(ca53_dmtck, "ca53-dmtck", &ap_apb.common.hw, 0x278,
			BIT(16), 0, 0);

static const struct clk_parent_data ca53_ts_parents[] = {
	{ .fw_name = "ext-32k" },
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_153m6.hw  },
};

static SPRD_MUX_CLK_DATA(ca53_ts_clk, "ca53-ts-clk", ca53_ts_parents, 0x27c,
			 0, 2, SC9832E_MUX_FLAG);

static SPRD_GATE_CLK_HW(djtag_tck, "djtag-tck", &ap_apb.common.hw, 0x280,
			BIT(16), 0, 0);

static const struct clk_parent_data emc_ref_parents[] = {
	{ .fw_name = "ext-6m5" },
	{ .fw_name = "ext-13m" },
	{ .fw_name = "ext-26m" },
};

static SPRD_MUX_CLK_DATA(emc_ref_clk, "emc-ref-clk", emc_ref_parents, 0x28c,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data cssys_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_96m.hw  },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_153m6.hw  },
	{ .hw = &twpll_256m.hw  },
};

static SPRD_COMP_CLK_DATA(cssys_clk, "cssys-clk", cssys_parents, 0x290,
			  0, 3, 8, 2, 0);

static const struct clk_parent_data tmr_parents[] = {
	{ .fw_name = "ext-32k" },
	{ .fw_name = "ext-4m3" },
};

static SPRD_MUX_CLK_DATA(tmr_clk, "tmr-clk", tmr_parents, 0x298,
			 0, 1, SC9832E_MUX_FLAG);

static SPRD_GATE_CLK_HW(dsi_test, "dsi-test", &ap_apb.common.hw, 0x2a0,
			BIT(16), 0, 0);

static const struct clk_parent_data sdphy_apb_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_48m.hw  },
};

static SPRD_MUX_CLK_DATA(sdphy_apb_clk, "sdphy-apb-clk", sdphy_apb_parents, 0x2b8,
			 0, 1, SC9832E_MUX_FLAG);

static const struct clk_parent_data aio_apb_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_48m.hw  },
};

static SPRD_COMP_CLK_DATA(aio_apb_clk, "aio-apb-clk", aio_apb_parents, 0x2c4,
			  0, 1, 8, 2, 0);

static SPRD_GATE_CLK_HW(dtck_hw, "dtck-hw", &ap_apb.common.hw, 0x2c8,
			BIT(16), 0, 0);

static const struct clk_parent_data ap_mm_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_96m.hw  },
	{ .hw = &twpll_128m.hw  },
};

static SPRD_COMP_CLK_DATA(ap_mm_clk, "ap-mm-clk", ap_mm_parents, 0x2cc,
			  0, 2, 8, 2, 0);

static const struct clk_parent_data ap_axi_parents[] = {
	{ .fw_name = "ext-26m" },
	{ .hw = &twpll_76m8.hw  },
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_256m.hw  },
};

static SPRD_MUX_CLK_DATA(ap_axi_clk, "ap-axi", ap_axi_parents, 0x2d0,
			 0, 2, SC9832E_MUX_FLAG);

static const struct clk_parent_data nic_gpu_parents[] = {
	{ .hw = &twpll_256m.hw  },
	{ .hw = &twpll_307m2.hw  },
	{ .hw = &twpll_384m.hw  },
	{ .hw = &twpll_512m.hw  },
	{ .hw = &gpll.common.hw  },
};

static SPRD_COMP_CLK_DATA(nic_gpu_clk, "nic-gpu-clk", nic_gpu_parents, 0x2d8,
			  0, 3, 8, 3, 0);

static const struct clk_parent_data mm_isp_parents[] = {
	{ .hw = &twpll_128m.hw  },
	{ .hw = &twpll_256m.hw  },
	{ .hw = &twpll_307m2.hw  },
	{ .hw = &twpll_384m.hw  },
	{ .hw = &isppll_468m.hw  },
};

static SPRD_MUX_CLK_DATA(mm_isp_clk, "mm-isp-clk", mm_isp_parents, 0x2dc,
			 0, 3, SC9832E_MUX_FLAG);

static struct sprd_clk_common *sc9832e_aon_clks[] = {
	&aon_apb.common,
	&adi_clk.common,
	&aux0_clk.common,
	&aux1_clk.common,
	&pwm0_clk.common,
	&pwm1_clk.common,
	&pwm2_clk.common,
	&pwm3_clk.common,
	&thm0_clk.common,
	&thm1_clk.common,
	&audif_clk.common,
	&aud_iis_da0.common,
	&aud_iis_ad0.common,
	&ca53_dap_clk.common,
	&ca53_dmtck.common,
	&ca53_ts_clk.common,
	&djtag_tck.common,
	&emc_ref_clk.common,
	&cssys_clk.common,
	&tmr_clk.common,
	&dsi_test.common,
	&sdphy_apb_clk.common,
	&aio_apb_clk.common,
	&dtck_hw.common,
	&ap_mm_clk.common,
	&ap_axi_clk.common,
	&nic_gpu_clk.common,
	&mm_isp_clk.common,
};

static struct clk_hw_onecell_data sc9832e_aon_clk_hws = {
	.hws	= {
		[CLK_AON_APB]		= &aon_apb.common.hw,
		[CLK_ADI]		= &adi_clk.common.hw,
		[CLK_AUX0]		= &aux0_clk.common.hw,
		[CLK_AUX1]		= &aux1_clk.common.hw,
		[CLK_PWM0]		= &pwm0_clk.common.hw,
		[CLK_PWM1]		= &pwm1_clk.common.hw,
		[CLK_PWM2]		= &pwm2_clk.common.hw,
		[CLK_PWM3]		= &pwm3_clk.common.hw,
		[CLK_THM0]		= &thm0_clk.common.hw,
		[CLK_THM1]		= &thm1_clk.common.hw,
		[CLK_AUDIF]		= &audif_clk.common.hw,
		[CLK_AUD_IIS_DA0]	= &aud_iis_da0.common.hw,
		[CLK_AUD_IIS_AD0]	= &aud_iis_ad0.common.hw,
		[CLK_CA53_DAP]		= &ca53_dap_clk.common.hw,
		[CLK_CA53_DMTCK]	= &ca53_dmtck.common.hw,
		[CLK_CA53_TS]		= &ca53_ts_clk.common.hw,
		[CLK_DJTAG_TCK]		= &djtag_tck.common.hw,
		[CLK_EMC_REF]		= &emc_ref_clk.common.hw,
		[CLK_CSSYS]		= &cssys_clk.common.hw,
		[CLK_TMR]		= &tmr_clk.common.hw,
		[CLK_DSI_TEST]		= &dsi_test.common.hw,
		[CLK_SDPHY_APB]		= &sdphy_apb_clk.common.hw,
		[CLK_AIO_APB]		= &aio_apb_clk.common.hw,
		[CLK_DTCK_HW]		= &dtck_hw.common.hw,
		[CLK_AP_MM]		= &ap_mm_clk.common.hw,
		[CLK_AP_AXI]		= &ap_axi_clk.common.hw,
		[CLK_NIC_GPU]		= &nic_gpu_clk.common.hw,
		[CLK_MM_ISP]		= &mm_isp_clk.common.hw,
	},
	.num	= CLK_AON_CLK_NUM,
};

static const struct sprd_clk_desc sc9832e_aon_clk_desc = {
	.clk_clks	= sc9832e_aon_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_aon_clks),
	.hw_clks	= &sc9832e_aon_clk_hws,
};

static SPRD_SC_GATE_CLK_FW_NAME(sim0_eb, "sim0-eb", "ext-26m", 0x0, 0x1000, BIT(0), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(iis0_eb, "iis0-eb", "ext-26m", 0x0, 0x1000, BIT(1), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(apb_reg_eb, "apb-reg-eb", "ext-26m", 0x0, 0x1000, BIT(2), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(spi0_eb, "spi0-eb", "ext-26m", 0x0, 0x1000, BIT(5), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(spi2_eb, "spi2-eb", "ext-26m", 0x0, 0x1000, BIT(7), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(i2c0_eb, "i2c0-eb", "ext-26m", 0x0, 0x1000, BIT(8), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(i2c1_eb, "i2c1-eb", "ext-26m", 0x0, 0x1000, BIT(9), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(i2c2_eb, "i2c2-eb", "ext-26m", 0x0, 0x1000, BIT(10), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(i2c3_eb, "i2c3-eb", "ext-26m", 0x0, 0x1000, BIT(11), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(i2c4_eb, "i2c4-eb", "ext-26m", 0x0, 0x1000, BIT(12), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(uart1_eb, "uart1-eb", "ap-apb", 0x0, 0x1000, BIT(14), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(sim0_32k_eb, "sim0-32k-eb", "ext-26m", 0x0, 0x1000, BIT(18), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(intc0_eb, "intc0-eb", "ext-26m", 0x0, 0x1000, BIT(19), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(intc1_eb, "intc1-eb", "ext-26m", 0x0, 0x1000, BIT(20), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(intc2_eb, "intc2-eb", "ext-26m", 0x0, 0x1000, BIT(21), 0, 0);
static SPRD_SC_GATE_CLK_FW_NAME(intc3_eb, "intc3-eb", "ext-26m", 0x0, 0x1000, BIT(22), 0, 0);

static struct sprd_clk_common *sc9832e_apapb_gate_clks[] = {
	&sim0_eb.common,
	&iis0_eb.common,
	&apb_reg_eb.common,
	&spi0_eb.common,
	&spi2_eb.common,
	&i2c0_eb.common,
	&i2c1_eb.common,
	&i2c2_eb.common,
	&i2c3_eb.common,
	&i2c4_eb.common,
	&uart1_eb.common,
	&sim0_32k_eb.common,
	&intc0_eb.common,
	&intc1_eb.common,
	&intc2_eb.common,
	&intc3_eb.common,
};

static struct clk_hw_onecell_data sc9832e_apapb_gate_hws = {
	.hws	= {
		[CLK_SIM0_EB]		= &sim0_eb.common.hw,
		[CLK_IIS0_EB]		= &iis0_eb.common.hw,
		[CLK_APB_REG_EB]	= &apb_reg_eb.common.hw,
		[CLK_SPI0_EB]		= &spi0_eb.common.hw,
		[CLK_SPI2_EB]		= &spi2_eb.common.hw,
		[CLK_I2C0_EB]		= &i2c0_eb.common.hw,
		[CLK_I2C1_EB]		= &i2c1_eb.common.hw,
		[CLK_I2C2_EB]		= &i2c2_eb.common.hw,
		[CLK_I2C3_EB]		= &i2c3_eb.common.hw,
		[CLK_I2C4_EB]		= &i2c4_eb.common.hw,
		[CLK_UART1_EB]		= &uart1_eb.common.hw,
		[CLK_SIM0_32K_EB]	= &sim0_32k_eb.common.hw,
		[CLK_INTC0_EB]		= &intc0_eb.common.hw,
		[CLK_INTC1_EB]		= &intc1_eb.common.hw,
		[CLK_INTC2_EB]		= &intc2_eb.common.hw,
		[CLK_INTC3_EB]		= &intc3_eb.common.hw,
	},
	.num	= CLK_AP_APB_GATE_NUM,
};

static const struct sprd_clk_desc sc9832e_apapb_gate_desc = {
	.clk_clks	= sc9832e_apapb_gate_clks,
	.num_clk_clks	= ARRAY_SIZE(sc9832e_apapb_gate_clks),
	.hw_clks	= &sc9832e_apapb_gate_hws,
};

static int sc9832e_clk_probe(struct platform_device *pdev)
{
	const struct sprd_clk_desc *desc;
	int ret;

	desc = device_get_match_data(&pdev->dev);
	if (!desc)
		return -ENODEV;

	ret = sprd_clk_regmap_init(pdev, desc);
	if (ret)
		return ret;

	return sprd_clk_probe(&pdev->dev, desc->hw_clks);
}

static const struct of_device_id sprd_sc9832e_clk_ids[] = {
	{ .compatible = "sprd,sc9832e-glbregs",		/* 0x402b0000 */
	  .data = &sc9832e_pmu_gate_desc },
	{ .compatible = "sprd,sc9832e-pll",			/* 0x403c0000 */
	  .data = &sc9832e_pll_desc },
	{ .compatible = "sprd,sc9832e-mpll",		/* 0x403f0000 */
	  .data = &sc9832e_mpll_desc },
	{ .compatible = "sprd,sc9832e-dpll",		/* 0x403d0000 */
	  .data = &sc9832e_dpll_desc },
	{ .compatible = "sprd,sc9832e-rpll",		/* 0x40410000 */
	  .data = &sc9832e_rpll_desc },
	{ .compatible = "sprd,sc9832e-apahb-gate",	/* 0x20e00000 */
	  .data = &sc9832e_apahb_gate_desc },
	{ .compatible = "sprd,sc9832e-aonapb-gate",	/* 0x402e0000 */
	  .data = &sc9832e_aonapb_gate_desc },
	{ .compatible = "sprd,sc9832e-ap-clk",		/* 0x21500000 */
	  .data = &sc9832e_ap_clk_desc },
	{ .compatible = "sprd,sc9832e-aon-clk",		/* 0x402d0000 */
	  .data = &sc9832e_aon_clk_desc },
	{ .compatible = "sprd,sc9832e-apapb-gate",	/* 0x71300000 */
	  .data = &sc9832e_apapb_gate_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_sc9832e_clk_ids);

static struct platform_driver sprd_sc9832e_clk_driver = {
	.probe	= sc9832e_clk_probe,
	.driver	= {
		.name	= "sc9832e-clk",
		.of_match_table	= sprd_sc9832e_clk_ids,
	},
};

module_platform_driver(sprd_sc9832e_clk_driver);

MODULE_DESCRIPTION("Spreadtrum SC9832E Clock Driver");
MODULE_LICENSE("GPL");
