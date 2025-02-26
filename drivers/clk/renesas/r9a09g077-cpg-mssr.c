// SPDX-License-Identifier: GPL-2.0
/*
 * r9a09g077 Clock Pulse Generator / Module Standby and Software Reset
 *
 * Copyright (C) 2025 Renesas Electronics Corp.
 *
 */

#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include <dt-bindings/clock/renesas,r9a09g077-cpg-mssr.h>
#include "renesas-cpg-mssr.h"

#define SCKCR		0x00
#define SCKCR2		0x04
#define SCKCR3		0x08
#define SCKCR4		0x0C
#define PMSEL		0x10
#define PMSEL_PLL0	BIT(0)
#define PMSEL_PLL2	BIT(2)
#define PMSEL_PLL3	BIT(3)
#define PLL0EN		BIT(0)
#define PLL2EN		BIT(0)
#define PLL3EN		BIT(0)
#define PLL0MON		0x20
#define PLL0EN_REG	0x30
#define PLL0_SSC_CTR	0x34
#define PLL1MON		0x40
#define LOCOCR		0x70
#define HIZCTRLEN	0x80
#define PLL2MON		0x90
#define PLL2EN_REG	0xA0
#define PLL2_SSC_CTR	0xAC
#define PLL3MON		0xB0
#define PLL3EN_REG	0xC0
#define PLL3_VCO_CTR0	0xC4
#define PLL3_VCO_CTR1	0xC8
#define PLL4MON		0xD0
#define PHYSEL		BIT(21)

#define MRCTLA		0x240
#define MRCTLE		0x250
#define MRCTLI		0x260
#define MRCTLM		0x270

#define DDIV_PACK(offset, bitpos, size) \
		(((offset) << 20) | ((bitpos) << 12) | ((size) << 8))

#define DIVCA55		DDIV_PACK(SCKCR2, 8, 4)
#define DIVCA55S	DDIV_PACK(SCKCR2, 12, 1)
#define DIVCR520	DDIV_PACK(SCKCR2, 2, 2)
#define DIVCR521	DDIV_PACK(SCKCR2, 0, 2)
#define DIVLCDC		DDIV_PACK(SCKCR3, 20, 3)
#define DIVCKIO		DDIV_PACK(SCKCR, 16, 3)
#define DIVETHPHY	DDIV_PACK(SCKCR, 21, 1)
#define DIVCANFD	DDIV_PACK(SCKCR, 20, 1)
#define DIVSPI0		DDIV_PACK(SCKCR3, 0, 2)
#define DIVSPI1		DDIV_PACK(SCKCR3, 2, 2)
#define DIVSPI2		DDIV_PACK(SCKCR3, 4, 2)
#define DIVSPI3		DDIV_PACK(SCKCR2, 16, 2)

#define SEL_PLL_PACK(offset, bitpos, size) \
	(((offset) << 20) | ((bitpos) << 12) | ((size) << 8))

#define SEL_PLL		SEL_PLL_PACK(SCKCR, 22, 1)

#define GET_SHIFT(val)		FIELD_GET(GENMASK(19, 12), val)
#define GET_WIDTH(val)		FIELD_GET(GENMASK(11, 8), val)
#define GET_REG_OFFSET(val)	FIELD_GET(GENMASK(31, 20), val)

enum clk_ids {
	/* Core Clock Outputs exported to DT */
	LAST_DT_CORE_CLK = R9A09G077_LCDC_CLKD,

	/* External Input Clocks */
	CLK_EXTAL,
	CLK_LOCO,

	/* Internal Core Clocks */
	CLK_PLL0,
	CLK_PLL1,
	CLK_PLL2,
	CLK_PLL3,
	CLK_PLL4,
	CLK_SEL_PLL0,
	CLK_SEL_CLK_PLL0,
	CLK_SEL_PLL1,
	CLK_SEL_CLK_PLL1,
	CLK_SEL_PLL2,
	CLK_SEL_CLK_PLL2,
	CLK_SEL_PLL4,
	CLK_SEL_CLK_PLL4,
	CLK_SEL_CLK_SRC,
	CLK_SEL_EXTAL,
	CLK_SEL_LOCO,
	CLK_PLL3_INPUT,

	/* Module Clocks */
	MOD_CLK_BASE,
};

static const struct clk_div_table dtable_1_2[] = {
	{0, 2},
	{15, 1},
	{0, 0},
};

/* Mux clock tables */
static const char * const sel_clk_pll0[] = { ".sel_loco", ".sel_pll0" };
static const char * const sel_clk_pll1[] = { ".sel_loco", ".sel_pll1" };
static const char * const sel_clk_pll4[] = { ".sel_loco", ".sel_pll4" };

static const struct cpg_core_clk r9a09g077_core_clks[] __initconst = {
	/* External Clock Inputs */
	DEF_INPUT("extal", CLK_EXTAL),
	DEF_INPUT("loco", CLK_LOCO),

	/* Internal Core Clocks */
	DEF_FIXED(".pll0", CLK_PLL0, CLK_EXTAL, 48, 1),
	DEF_FIXED(".pll1", CLK_PLL1, CLK_EXTAL, 40, 1),
	DEF_FIXED(".pll4", CLK_PLL4, CLK_EXTAL, 96, 1),
	DEF_FIXED(".sel_pll0", CLK_SEL_PLL0, CLK_PLL0, 1, 1),
	DEF_MUX(".sel_clk_pll0", CLK_SEL_CLK_PLL0, SEL_PLL,
		sel_clk_pll0, ARRAY_SIZE(sel_clk_pll0), 0, CLK_MUX_READ_ONLY),
	DEF_FIXED(".sel_pll1", CLK_SEL_PLL1, CLK_PLL1, 1, 1),
	DEF_MUX(".sel_clk_pll1", CLK_SEL_CLK_PLL1, SEL_PLL,
		sel_clk_pll1, ARRAY_SIZE(sel_clk_pll1), 0, CLK_MUX_READ_ONLY),
	DEF_FIXED(".sel_pll4", CLK_SEL_PLL4, CLK_PLL4, 1, 1),
	DEF_MUX(".sel_clk_pll4", CLK_SEL_CLK_PLL4, SEL_PLL,
		sel_clk_pll4, ARRAY_SIZE(sel_clk_pll4), 0, CLK_MUX_READ_ONLY),

	/* Core output clk */
	DEF_DIV("CA55", R9A09G077_CA55, CLK_SEL_CLK_PLL0, DIVCA55,
		dtable_1_2, CLK_DIVIDER_HIWORD_MASK, 1),
	DEF_FIXED("PCLKM", R9A09G077_PCLKM, CLK_SEL_CLK_PLL1, 1, 8),
	DEF_FIXED("PCLKGPTL", R9A09G077_PCLKGPTL, CLK_SEL_CLK_PLL1, 1, 2),
};

static const struct mssr_mod_clk r9a09g077_mod_clks[] __initconst = {
	DEF_MOD("sci0", 108, R9A09G077_PCLKM),
};

static struct clk * __init
r9a09g077_cpg_div_clk_register(struct device *dev,
			       const struct cpg_core_clk *core,
			       void __iomem *base,
			       struct cpg_mssr_pub *pub)
{
	const struct clk *parent;
	const char *parent_name;
	struct clk_hw *clk_hw;

	parent = pub->clks[core->parent];

	if (IS_ERR(parent))
		return ERR_CAST(parent);

	parent_name = __clk_get_name(parent);

	if (core->dtable)
		clk_hw = clk_hw_register_divider_table(dev, core->name,
						       parent_name, 0,
						       base + GET_REG_OFFSET(core->conf),
						       GET_SHIFT(core->conf),
						       GET_WIDTH(core->conf),
						       core->flag,
						       core->dtable,
						       &pub->rmw_lock);
	else
		clk_hw = clk_hw_register_divider(dev, core->name,
						 parent_name, 0,
						 base + GET_REG_OFFSET(core->conf),
						 GET_SHIFT(core->conf),
						 GET_WIDTH(core->conf),
						 core->flag, &pub->rmw_lock);

	if (IS_ERR(clk_hw))
		return ERR_CAST(clk_hw);

	return clk_hw->clk;

}

static struct clk * __init
r9a09g077_cpg_mux_clk_register(struct device *dev,
			       const struct cpg_core_clk *core,
			       void __iomem *base,
			       struct cpg_mssr_pub *pub)
{
	struct clk_hw *clk_hw;

	clk_hw = devm_clk_hw_register_mux(dev, core->name,
					  core->parent_names, core->num_parents,
					  core->flag,
					  base + GET_REG_OFFSET(core->conf),
					  GET_SHIFT(core->conf),
					  GET_WIDTH(core->conf),
					  core->mux_flags, &pub->rmw_lock);
	return clk_hw->clk;
}

static struct clk * __init
r9a09g077_cpg_clk_register(struct device *dev,
			   const struct cpg_core_clk *core,
			   const struct cpg_mssr_info *info,
			   struct cpg_mssr_pub *pub)
{
	void __iomem *base = core->sel_base ? pub->base1 : pub->base0;

	switch (core->type) {
	case CLK_TYPE_DIV:
		return r9a09g077_cpg_div_clk_register(dev, core, base, pub);
	case CLK_TYPE_MUX:
		return r9a09g077_cpg_mux_clk_register(dev, core, base, pub);
	default:
		return ERR_PTR(-EINVAL);
	}
}

const struct cpg_mssr_info r9a09g077_cpg_mssr_info = {
	/* Core Clocks */
	.core_clks = r9a09g077_core_clks,
	.num_core_clks = ARRAY_SIZE(r9a09g077_core_clks),
	.last_dt_core_clk = LAST_DT_CORE_CLK,
	.num_total_core_clks = MOD_CLK_BASE,

	/* Module Clocks */
	.mod_clks = r9a09g077_mod_clks,
	.num_mod_clks = ARRAY_SIZE(r9a09g077_mod_clks),
	.num_hw_mod_clks = 12 * 32,

	.reg_layout = CLK_REG_LAYOUT_RZ_T2H,
	.cpg_clk_register = r9a09g077_cpg_clk_register,
};
