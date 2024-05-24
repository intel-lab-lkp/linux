// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/V2H(P) CPG driver
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include <dt-bindings/clock/r9a09g057-cpg.h>

#include "rzv2h-cpg.h"

enum pll_clk {
	PLLCM33,
	PLLCLN,
	PLLDTY,
	PLLCA55,
	PLLVDO,
	PLLETH,
	PLLDSI,
	PLLDDR0,
	PLLDDR1,
	PLLGPU,
	PLLDRP,
};

static int pll_clk1_offset[] = { -EINVAL, -EINVAL, -EINVAL, 0x64, -EINVAL,
			       -EINVAL, 0xC4, -EINVAL, -EINVAL, 0x124, 0x144 };
static int pll_clk2_offset[] = { -EINVAL, -EINVAL, -EINVAL, 0x68, -EINVAL,
			       -EINVAL, 0xC8, -EINVAL, -EINVAL, 0x128, 0x148 };

enum clk_ids {
	/* External Input Clocks */
	CLK_EXTAL,
	CLK_XINMAINCLK,
	/* Internal Core Clocks */
	CLK_PLLCM33,
	CLK_PLLCM33_DIV16,

	CLK_PLLCA55,
	/* Module Clocks */
	MOD_CLK_BASE,
};

static const struct cpg_core_clk r9a09g057_core_clks[] __initconst = {
	/* External Clock Inputs */
	DEF_INPUT("extal", CLK_EXTAL),

	/* Internal Core Clocks */
	DEF_FIXED(".main", CLK_XINMAINCLK, CLK_EXTAL, 1, 1),
	DEF_FIXED(".pllcm33", CLK_PLLCM33, CLK_EXTAL, 200, 3),
	DEF_FIXED(".pllcm33_div16", CLK_PLLCM33_DIV16, CLK_PLLCM33, 1, 16),

	DEF_PLL(".pllca55", CLK_PLLCA55, CLK_EXTAL, PLLCA55),
};

static int rzv2h_pll_get_clk1_offset(int clk)
{
	if (clk < 0 || clk >= ARRAY_SIZE(pll_clk1_offset))
		return -EINVAL;

	return pll_clk1_offset[clk];
}

static int rzv2h_pll_get_clk2_offset(int clk)
{
	if (clk < 0 || clk >= ARRAY_SIZE(pll_clk2_offset))
		return -EINVAL;

	return pll_clk2_offset[clk];
}

static struct rzv2h_mod_clk r9a09g057_mod_clks[] = {
	DEF_MOD("scif_0_clk_pck",		R9A09G057_SCIF_0_CLK_PCK, CLK_PLLCM33_DIV16,
						0x620, 15, 0x810, 15),
};

static struct rzv2h_reset r9a09g057_resets[] = {
	DEF_RST(R9A09G057_SCIF_0_RST_SYSTEM_N,		0x924,	5, 0xA10, 6),
};

static const unsigned int r9a09g057_crit_mod_clks[] __initconst = {
	MOD_CLK_BASE + R9A09G057_ICU_0_PCLK_I,
	MOD_CLK_BASE + R9A09G057_GIC_0_GICCLK,
};

const struct rzv2h_cpg_info r9a09g057_cpg_info = {
	/* Core Clocks */
	.core_clks = r9a09g057_core_clks,
	.num_core_clks = ARRAY_SIZE(r9a09g057_core_clks),
	.num_total_core_clks = MOD_CLK_BASE,

	/* Critical Module Clocks */
	.crit_mod_clks = r9a09g057_crit_mod_clks,
	.num_crit_mod_clks = ARRAY_SIZE(r9a09g057_crit_mod_clks),

	/* Module Clocks */
	.mod_clks = r9a09g057_mod_clks,
	.num_mod_clks = ARRAY_SIZE(r9a09g057_mod_clks),
	.num_hw_mod_clks = R9A09G057_TZCDDR_1_CLK400DG_ACP_ACLK4 + 1,

	/* Resets */
	.resets = r9a09g057_resets,
	.num_resets = R9A09G057_DRPAI_0_ARESETN + 1, /* Last reset ID + 1 */

	.pll_get_clk1_offset = &rzv2h_pll_get_clk1_offset,
	.pll_get_clk2_offset = &rzv2h_pll_get_clk2_offset,
};
