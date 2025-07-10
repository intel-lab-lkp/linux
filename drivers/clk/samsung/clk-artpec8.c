// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022-2025 Samsung Electronics Co., Ltd.
 *             https://www.samsung.com
 * Copyright (c) 2022-2025  Axis Communications AB.
 *             https://www.axis.com
 *
 * Common Clock Framework support for ARTPEC-8 SoC.
 */

#include <linux/clk-provider.h>
#include <dt-bindings/clock/axis,artpec8-clk.h>

#include "clk.h"

/* NOTE: Must be equal to the last clock ID increased by one */
#define CMU_IMEM_NR_CLK			(MOUT_IMEM_JPEG_USER + 1)

/* Register Offset definitions for CMU_IMEM (0x10010000) */
#define PLL_CON0_MUX_CLK_IMEM_ACLK_USER			0x0100
#define PLL_CON0_MUX_CLK_IMEM_JPEG_USER			0x0120
#define MUX_CLK_IMEM_GIC_CA53				0x1000
#define MUX_CLK_IMEM_GIC_CA5				0x1008

static const unsigned long cmu_imem_clk_regs[] __initconst = {
	PLL_CON0_MUX_CLK_IMEM_ACLK_USER,
	PLL_CON0_MUX_CLK_IMEM_JPEG_USER,
	MUX_CLK_IMEM_GIC_CA53,
	MUX_CLK_IMEM_GIC_CA5,
};

PNAME(mout_imem_aclk_user_p) = { "fin_pll", "dout_clkcmu_imem_aclk" };
PNAME(mout_imem_gic_ca53_p) = { "mout_imem_aclk_user", "fin_pll" };
PNAME(mout_imem_gic_ca5_p) = { "mout_imem_aclk_user", "fin_pll" };
PNAME(mout_imem_jpeg_user_p) = { "fin_pll", "dout_clkcmu_imem_jpeg" };

static const struct samsung_mux_clock cmu_imem_mux_clks[] __initconst = {
	MUX(MOUT_IMEM_ACLK_USER, "mout_imem_aclk_user",
	    mout_imem_aclk_user_p, PLL_CON0_MUX_CLK_IMEM_ACLK_USER, 4, 1),
	MUX(MOUT_IMEM_GIC_CA53, "mout_imem_gic_ca53",
	    mout_imem_gic_ca53_p, MUX_CLK_IMEM_GIC_CA53, 0, 1),
	MUX(MOUT_IMEM_GIC_CA5, "mout_imem_gic_ca5",
	    mout_imem_gic_ca5_p, MUX_CLK_IMEM_GIC_CA5, 0, 1),
	MUX(MOUT_IMEM_JPEG_USER, "mout_imem_jpeg_user",
	    mout_imem_jpeg_user_p, PLL_CON0_MUX_CLK_IMEM_JPEG_USER, 4, 1),
};

static const struct samsung_cmu_info cmu_imem_info __initconst = {
	.mux_clks		= cmu_imem_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(cmu_imem_mux_clks),
	.nr_clk_ids		= CMU_IMEM_NR_CLK,
	.clk_regs		= cmu_imem_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(cmu_imem_clk_regs),
};

static void __init artpec8_clk_cmu_imem_init(struct device_node *np)
{
	samsung_cmu_register_one(np, &cmu_imem_info);
}

CLK_OF_DECLARE(artpec8_clk_cmu_imem, "axis,artpec8-cmu-imem",
	       artpec8_clk_cmu_imem_init);
