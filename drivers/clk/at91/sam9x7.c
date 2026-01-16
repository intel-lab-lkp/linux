// SPDX-License-Identifier: GPL-2.0
/*
 * SAM9X7 PMC code.
 *
 * Copyright (C) 2023 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Varshini Rajendran <varshini.rajendran@microchip.com>
 *
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/slab.h>

#include <dt-bindings/clock/at91.h>

#include "pmc.h"

static DEFINE_SPINLOCK(pmc_pll_lock);
static DEFINE_SPINLOCK(mck_lock);

/**
 * enum pll_ids - PLL clocks identifiers
 * @PLL_ID_PLLA:	PLLA identifier
 * @PLL_ID_UPLL:	UPLL identifier
 * @PLL_ID_AUDIO:	Audio PLL identifier
 * @PLL_ID_LVDS:	LVDS PLL identifier
 * @PLL_ID_PLLA_DIV2:	PLLA DIV2 identifier
 * @PLL_ID_MAX:		Max PLL Identifier
 */
enum pll_ids {
	PLL_ID_PLLA,
	PLL_ID_UPLL,
	PLL_ID_AUDIO,
	PLL_ID_LVDS,
	PLL_ID_MAX,
};

/*
 * PLL component identifier
 * @PLL_COMPID_FRAC: Fractional PLL component identifier
 * @PLL_COMPID_DIV0: 1st PLL divider component identifier
 * @PLL_COMPID_DIV1: 2nd PLL divider component identifier
 */
enum pll_component_id {
	PLL_COMPID_FRAC,
	PLL_COMPID_DIV0,
	PLL_COMPID_DIV1,
	PLL_COMPID_MAX,
};

/**
 * enum pll_type - PLL type identifiers
 * @PLL_TYPE_FRAC:	fractional PLL identifier
 * @PLL_TYPE_DIV:	divider PLL identifier
 */
enum pll_type {
	PLL_TYPE_FRAC,
	PLL_TYPE_DIV,
};

static const struct clk_master_characteristics mck_characteristics = {
	.output = { .min = 32000000, .max = 266666667 },
	.divisors = { 1, 2, 4, 3, 5},
	.have_div3_pres = 1,
};

static const struct clk_master_layout sam9x7_master_layout = {
	.mask = 0x373,
	.pres_shift = 4,
	.offset = 0x28,
};

/* Fractional PLL core output range. */
static const struct clk_range plla_core_outputs[] = {
	{ .min = 800000000, .max = 1600000000 },
};

static const struct clk_range upll_core_outputs[] = {
	{ .min = 600000000, .max = 960000000 },
};

static const struct clk_range lvdspll_core_outputs[] = {
	{ .min = 600000000, .max = 1200000000 },
};

static const struct clk_range audiopll_core_outputs[] = {
	{ .min = 600000000, .max = 1200000000 },
};

static const struct clk_range plladiv2_core_outputs[] = {
	{ .min = 800000000, .max = 1600000000 },
};

/* Fractional PLL output range. */
static const struct clk_range plla_outputs[] = {
	{ .min = 400000000, .max = 800000000 },
};

static const struct clk_range upll_outputs[] = {
	{ .min = 300000000, .max = 480000000 },
};

static const struct clk_range lvdspll_outputs[] = {
	{ .min = 175000000, .max = 550000000 },
};

static const struct clk_range audiopll_outputs[] = {
	{ .min = 0, .max = 300000000 },
};

static const struct clk_range plladiv2_outputs[] = {
	{ .min = 200000000, .max = 400000000 },
};

/* PLL characteristics. */
static const struct clk_pll_characteristics plla_characteristics = {
	.input = { .min = 20000000, .max = 50000000 },
	.num_output = ARRAY_SIZE(plla_outputs),
	.output = plla_outputs,
	.core_output = plla_core_outputs,
	.acr = UL(0x00020010), /* Old ACR_DEFAULT_PLLA value */
};

static const struct clk_pll_characteristics upll_characteristics = {
	.input = { .min = 20000000, .max = 50000000 },
	.num_output = ARRAY_SIZE(upll_outputs),
	.output = upll_outputs,
	.core_output = upll_core_outputs,
	.upll = true,
	.acr = UL(0x12023010), /* fIN=[20 MHz, 32 MHz] */
};

static const struct clk_pll_characteristics lvdspll_characteristics = {
	.input = { .min = 20000000, .max = 50000000 },
	.num_output = ARRAY_SIZE(lvdspll_outputs),
	.output = lvdspll_outputs,
	.core_output = lvdspll_core_outputs,
	.acr = UL(0x12023010), /* fIN=[20 MHz, 32 MHz] */
};

static const struct clk_pll_characteristics audiopll_characteristics = {
	.input = { .min = 20000000, .max = 50000000 },
	.num_output = ARRAY_SIZE(audiopll_outputs),
	.output = audiopll_outputs,
	.core_output = audiopll_core_outputs,
	.acr = UL(0x12023010), /* fIN=[20 MHz, 32 MHz] */
};

static const struct clk_pll_characteristics plladiv2_characteristics = {
	.input = { .min = 20000000, .max = 50000000 },
	.num_output = ARRAY_SIZE(plladiv2_outputs),
	.output = plladiv2_outputs,
	.core_output = plladiv2_core_outputs,
	.acr = UL(0x00020010),  /* Old ACR_DEFAULT_PLLA value */
};

/* Layout for fractional PLL ID PLLA. */
static const struct clk_pll_layout plla_frac_layout = {
	.mul_mask = GENMASK(31, 24),
	.frac_mask = GENMASK(21, 0),
	.mul_shift = 24,
	.frac_shift = 0,
	.div2 = 1,
};

/* Layout for fractional PLLs. */
static const struct clk_pll_layout pll_frac_layout = {
	.mul_mask = GENMASK(31, 24),
	.frac_mask = GENMASK(21, 0),
	.mul_shift = 24,
	.frac_shift = 0,
};

/* Layout for DIV PLLs. */
static const struct clk_pll_layout pll_divpmc_layout = {
	.div_mask = GENMASK(7, 0),
	.endiv_mask = BIT(29),
	.div_shift = 0,
	.endiv_shift = 29,
};

/* Layout for DIV PLL ID PLLADIV2. */
static const struct clk_pll_layout plladiv2_divpmc_layout = {
	.div_mask = GENMASK(7, 0),
	.endiv_mask = BIT(29),
	.div_shift = 0,
	.endiv_shift = 29,
	.div2 = 1,
};

/* Layout for DIVIO dividers. */
static const struct clk_pll_layout pll_divio_layout = {
	.div_mask	= GENMASK(19, 12),
	.endiv_mask	= BIT(30),
	.div_shift	= 12,
	.endiv_shift	= 30,
};

/*
 * SAM9X7 PLL possible parents
 * @SAM9X7_PLL_PARENT_MAINCK: MAINCK is PLL a parent
 * @SAM9X7_PLL_PARENT_MAIN_XTAL: MAIN XTAL is a PLL parent
 * @SAM9X7_PLL_PARENT_FRACCK: Frac PLL is a PLL parent (for PLL dividers)
 */
enum sam9x7_pll_parent {
	SAM9X7_PLL_PARENT_MAINCK,
	SAM9X7_PLL_PARENT_MAIN_XTAL,
	SAM9X7_PLL_PARENT_FRACCK
};

/*
 * PLL clocks description
 * @n:		clock name
 * @p:		clock parent
 * @l:		clock layout
 * @t:		clock type
 * @c:		pll characteristics
 * @hw:		pointer to clk_hw
 * @f:		clock flags
 * @eid:	export index in sam9x7->chws[] array
 */
static struct {
	const char *n;
	const struct clk_pll_layout *l;
	u8 t;
	const struct clk_pll_characteristics *c;
	struct clk_hw *hw;
	unsigned long f;
	enum sam9x7_pll_parent p;
	u8 eid;
} sam9x7_plls[][PLL_COMPID_MAX] = {
	[PLL_ID_PLLA] = {
		[PLL_COMPID_FRAC] = {
			.n = "plla_fracck",
			.p = SAM9X7_PLL_PARENT_MAINCK,
			.l = &plla_frac_layout,
			.t = PLL_TYPE_FRAC,
			/*
			 * This feeds plla_divpmcck which feeds CPU. It should
			 * not be disabled.
			 */
			.f = CLK_IS_CRITICAL | CLK_SET_RATE_GATE,
			.c = &plla_characteristics,
		},

		[PLL_COMPID_DIV0] = {
			.n = "plla_divpmcck",
			.p = SAM9X7_PLL_PARENT_FRACCK,
			.l = &pll_divpmc_layout,
			.t = PLL_TYPE_DIV,
			/* This feeds CPU. It should not be disabled */
			.f = CLK_IS_CRITICAL | CLK_SET_RATE_GATE,
			.eid = PMC_PLLACK,
			.c = &plla_characteristics,
		},

		[PLL_COMPID_DIV1] = {
			.n = "plla_div2pmcck",
			.p = SAM9X7_PLL_PARENT_FRACCK,
			.l = &plladiv2_divpmc_layout,
			/*
			 * This may feed critical parts of the system like timers.
			 * It should not be disabled.
			 */
			.f = CLK_IS_CRITICAL | CLK_SET_RATE_GATE,
			.c = &plladiv2_characteristics,
			.eid = PMC_PLLADIV2,
			.t = PLL_TYPE_DIV,
		},
	},

	[PLL_ID_UPLL] = {
		[PLL_COMPID_FRAC] = {
			.n = "upll_fracck",
			.p = SAM9X7_PLL_PARENT_MAIN_XTAL,
			.l = &pll_frac_layout,
			.t = PLL_TYPE_FRAC,
			.f = CLK_SET_RATE_GATE,
			.c = &upll_characteristics,
		},

		[PLL_COMPID_DIV0] = {
			.n = "upll_divpmcck",
			.p = SAM9X7_PLL_PARENT_FRACCK,
			.l = &pll_divpmc_layout,
			.t = PLL_TYPE_DIV,
			.f = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE |
			     CLK_SET_RATE_PARENT,
			.eid = PMC_UTMI,
			.c = &upll_characteristics,
		},
	},

	[PLL_ID_AUDIO] = {
		[PLL_COMPID_FRAC] = {
			.n = "audiopll_fracck",
			.p = SAM9X7_PLL_PARENT_MAIN_XTAL,
			.l = &pll_frac_layout,
			.f = CLK_SET_RATE_GATE,
			.c = &audiopll_characteristics,
			.t = PLL_TYPE_FRAC,
		},

		[PLL_COMPID_DIV0] = {
			.n = "audiopll_divpmcck",
			.p = SAM9X7_PLL_PARENT_FRACCK,
			.l = &pll_divpmc_layout,
			.f = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE |
			     CLK_SET_RATE_PARENT,
			.c = &audiopll_characteristics,
			.eid = PMC_AUDIOPMCPLL,
			.t = PLL_TYPE_DIV,
		},

		[PLL_COMPID_DIV1] = {
			.n = "audiopll_diviock",
			.p = SAM9X7_PLL_PARENT_FRACCK,
			.l = &pll_divio_layout,
			.f = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE |
			     CLK_SET_RATE_PARENT,
			.c = &audiopll_characteristics,
			.eid = PMC_AUDIOIOPLL,
			.t = PLL_TYPE_DIV,
		},
	},

	[PLL_ID_LVDS] = {
		[PLL_COMPID_FRAC] = {
			.n = "lvdspll_fracck",
			.p = SAM9X7_PLL_PARENT_MAIN_XTAL,
			.l = &pll_frac_layout,
			.f = CLK_SET_RATE_GATE,
			.c = &lvdspll_characteristics,
			.t = PLL_TYPE_FRAC,
		},

		[PLL_COMPID_DIV0] = {
			.n = "lvdspll_divpmcck",
			.p = SAM9X7_PLL_PARENT_FRACCK,
			.l = &pll_divpmc_layout,
			.f = CLK_SET_RATE_GATE | CLK_SET_PARENT_GATE |
			     CLK_SET_RATE_PARENT,
			.c = &lvdspll_characteristics,
			.eid = PMC_LVDSPLL,
			.t = PLL_TYPE_DIV,
		},
	},
};

static const struct clk_programmable_layout sam9x7_programmable_layout = {
	.pres_mask = 0xff,
	.pres_shift = 8,
	.css_mask = 0x1f,
	.have_slck_mck = 0,
	.is_pres_direct = 1,
};

static const struct clk_pcr_layout sam9x7_pcr_layout = {
	.offset = 0x88,
	.cmd = BIT(31),
	.gckcss_mask = GENMASK(12, 8),
	.pid_mask = GENMASK(6, 0),
};

static struct {
	char *n;
	struct clk_hw *parent_hw;
	u8 id;
	unsigned long flags;
} sam9x7_systemck[] = {
	/*
	 * ddrck feeds DDR controller and is enabled by bootloader thus we need
	 * to keep it enabled in case there is no Linux consumer for it.
	 */
	{ .n = "ddrck",		.id = 2,	.flags = CLK_IS_CRITICAL },
	{ .n = "uhpck",		.id = 6 },
	{ .n = "pck0",		.id = 8 },
	{ .n = "pck1",		.id = 9 },
};

/*
 * Peripheral clocks description
 * @n:		clock name
 * @f:		clock flags
 * @id:		peripheral id
 */
static const struct {
	char *n;
	unsigned long f;
	u8 id;
} sam9x7_periphck[] = {
	{ .n = "pioA_clk",	.id = 2, },
	{ .n = "pioB_clk",	.id = 3, },
	{ .n = "pioC_clk",	.id = 4, },
	{ .n = "flex0_clk",	.id = 5, },
	{ .n = "flex1_clk",	.id = 6, },
	{ .n = "flex2_clk",	.id = 7, },
	{ .n = "flex3_clk",	.id = 8, },
	{ .n = "flex6_clk",	.id = 9, },
	{ .n = "flex7_clk",	.id = 10, },
	{ .n = "flex8_clk",	.id = 11, },
	{ .n = "sdmmc0_clk",	.id = 12, },
	{ .n = "flex4_clk",	.id = 13, },
	{ .n = "flex5_clk",	.id = 14, },
	{ .n = "flex9_clk",	.id = 15, },
	{ .n = "flex10_clk",	.id = 16, },
	{ .n = "tcb0_clk",	.id = 17, },
	{ .n = "pwm_clk",	.id = 18, },
	{ .n = "adc_clk",	.id = 19, },
	{ .n = "dma0_clk",	.id = 20, },
	{ .n = "uhphs_clk",	.id = 22, },
	{ .n = "udphs_clk",	.id = 23, },
	{ .n = "macb0_clk",	.id = 24, },
	{ .n = "lcd_clk",	.id = 25, },
	{ .n = "sdmmc1_clk",	.id = 26, },
	{ .n = "ssc_clk",	.id = 28, },
	{ .n = "can0_clk",	.id = 29, },
	{ .n = "can1_clk",	.id = 30, },
	{ .n = "flex11_clk",	.id = 32, },
	{ .n = "flex12_clk",	.id = 33, },
	{ .n = "i2s_clk",	.id = 34, },
	{ .n = "qspi_clk",	.id = 35, },
	{ .n = "gfx2d_clk",	.id = 36, },
	{ .n = "pit64b0_clk",	.id = 37, },
	{ .n = "trng_clk",	.id = 38, },
	{ .n = "aes_clk",	.id = 39, },
	{ .n = "tdes_clk",	.id = 40, },
	{ .n = "sha_clk",	.id = 41, },
	{ .n = "classd_clk",	.id = 42, },
	{ .n = "isi_clk",	.id = 43, },
	{ .n = "pioD_clk",	.id = 44, },
	{ .n = "tcb1_clk",	.id = 45, },
	{ .n = "dbgu_clk",	.id = 47, },
	{ .n = "pmecc_clk",	.id = 48, },
	/*
	 * mpddr_clk feeds DDR controller and is enabled by bootloader thus we
	 * need to keep it enabled in case there is no Linux consumer for it.
	 */
	{ .n = "mpddr_clk",	.id = 49,	.f = CLK_IS_CRITICAL },
	{ .n = "csi2dc_clk",	.id = 52, },
	{ .n = "csi4l_clk",	.id = 53, },
	{ .n = "dsi4l_clk",	.id = 54, },
	{ .n = "lvdsc_clk",	.id = 56, },
	{ .n = "pit64b1_clk",	.id = 58, },
	{ .n = "puf_clk",	.id = 59, },
	{ .n = "gmactsu_clk",	.id = 67, },
};

/*
 * Generic clock description
 * @n:			clock name
 * @pp:			PLL parents (entry formed by PLL components identifiers
 *			(see enum pll_component_id))
 * @pp_mux_table:	PLL parents mux table
 * @r:			clock output range
 * @pp_chg_id:		id in parent array of changeable PLL parent
 * @pp_count:		PLL parents count
 * @id:			clock id
 */
static const struct {
	const char *n;
	struct {
			int pll_id;
			int pll_compid;
	} pp[8];
	const char pp_mux_table[8];
	struct clk_range r;
	int pp_chg_id;
	u8 pp_count;
	u8 id;
} sam9x7_gck[] = {
	{
		.n = "flex0_gclk",
		.id = 5,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex1_gclk",
		.id = 6,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex2_gclk",
		.id = 7,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex3_gclk",
		.id = 8,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex6_gclk",
		.id = 9,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex7_gclk",
		.id = 10,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex8_gclk",
		.id = 11,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "sdmmc0_gclk",
		.id = 12,
		.r = { .max = 105000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex4_gclk",
		.id = 13,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex5_gclk",
		.id = 14,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex9_gclk",
		.id = 15,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex10_gclk",
		.id = 16,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "tcb0_gclk",
		.id = 17,
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "adc_gclk",
		.id = 19,
		.pp = { PLL_IDS_TO_ARR_ENTRY(UPLL, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 5, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "lcd_gclk",
		.id = 25,
		.r = { .max = 75000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "sdmmc1_gclk",
		.id = 26,
		.r = { .max = 105000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "mcan0_gclk",
		.id = 29,
		.r = { .max = 80000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(UPLL, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 5, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "mcan1_gclk",
		.id = 30,
		.r = { .max = 80000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(UPLL, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 5, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex11_gclk",
		.id = 32,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "flex12_gclk",
		.id = 33,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "i2s_gclk",
		.id = 34,
		.r = { .max = 100000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "qspi_gclk",
		.id = 35,
		.r = { .max = 200000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "pit64b0_gclk",
		.id = 37,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "classd_gclk",
		.id = 42,
		.r = { .max = 100000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "tcb1_gclk",
		.id = 45,
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "dbgu_gclk",
		.id = 47,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "mipiphy_gclk",
		.id = 55,
		.r = { .max = 27000000 },
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "pit64b1_gclk",
		.id = 58,
		.pp = { PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 8, },
		.pp_count = 1,
		.pp_chg_id = INT_MIN,
	},

	{
		.n = "gmac_gclk",
		.id = 67,
		.pp = { PLL_IDS_TO_ARR_ENTRY(AUDIO, DIV0), PLL_IDS_TO_ARR_ENTRY(PLLA, DIV1), },
		.pp_mux_table = { 6, 8, },
		.pp_count = 2,
		.pp_chg_id = INT_MIN,
	},
};

static void __init sam9x7_pmc_setup(struct device_node *np)
{
	struct clk_range range = CLK_RANGE(0, 0);
	const char *main_xtal_name;
	struct pmc_data *sam9x7_pmc;
	const char *parent_names[9];
	void **clk_mux_buffer = NULL;
	int clk_mux_buffer_size = 0;
	struct regmap *regmap;
	struct clk_hw *hw, *main_rc_hw, *main_osc_hw, *main_xtal_hw;
	struct clk_hw *td_slck_hw, *md_slck_hw, *usbck_hw;
	struct clk_hw *parent_hws[9];
	int i, j;

	i = of_property_match_string(np, "clock-names", "main_xtal");

	if (i < 0)
		return;
	main_xtal_name = of_clk_get_parent_name(np, i);

	td_slck_hw = __clk_get_hw(of_clk_get_by_name(np, "td_slck"));
	md_slck_hw = __clk_get_hw(of_clk_get_by_name(np, "md_slck"));

	if (!td_slck_hw || !md_slck_hw)
		return;

	regmap = device_node_to_regmap(np);
	if (IS_ERR(regmap))
		return;

	sam9x7_pmc = pmc_data_allocate(PMC_LVDSPLL + 1,
				       nck(sam9x7_systemck),
				       nck(sam9x7_periphck),
				       nck(sam9x7_gck), 8);
	if (!sam9x7_pmc)
		return;

	clk_mux_buffer = kmalloc(sizeof(void *) *
				 (ARRAY_SIZE(sam9x7_gck)),
				 GFP_KERNEL);
	if (!clk_mux_buffer)
		goto err_free;

	main_rc_hw = at91_clk_register_main_rc_osc(regmap, "main_rc_osc", 12000000,
						   50000000);
	if (IS_ERR(main_rc_hw))
		goto err_free;

	main_osc_hw = at91_clk_register_main_osc(regmap, "main_osc", NULL,
						 &AT91_CLK_PD_NAME(main_xtal_name), 0);
	if (IS_ERR(main_osc_hw))
		goto err_free;

	parent_hws[0] = main_rc_hw;
	parent_hws[1] = main_osc_hw;
	hw = at91_clk_register_sam9x5_main(regmap, "mainck", NULL, parent_hws, 2);
	if (IS_ERR(hw))
		goto err_free;

	sam9x7_pmc->chws[PMC_MAIN] = hw;

	for (i = 0; i < PLL_ID_MAX; i++) {
		for (j = 0; j < PLL_COMPID_MAX; j++) {
			struct clk_hw *parent_hw;

			if (!sam9x7_plls[i][j].n)
				continue;

			switch (sam9x7_plls[i][j].t) {
			case PLL_TYPE_FRAC:
				switch (sam9x7_plls[i][j].p) {
				case SAM9X7_PLL_PARENT_MAINCK:
					parent_hw = sam9x7_pmc->chws[PMC_MAIN];
					break;
				case SAM9X7_PLL_PARENT_MAIN_XTAL:
					parent_hw = main_xtal_hw;
					break;
				default:
					/* Should not happen. */
					parent_hw = NULL;
					break;
				}

				hw = sam9x60_clk_register_frac_pll(regmap,
								   &pmc_pll_lock,
								   sam9x7_plls[i][j].n,
								   NULL, parent_hw, i,
								   sam9x7_plls[i][j].c,
								   sam9x7_plls[i][j].l,
								   sam9x7_plls[i][j].f);
				break;

			case PLL_TYPE_DIV:
				hw = sam9x60_clk_register_div_pll(regmap,
								  &pmc_pll_lock,
								  sam9x7_plls[i][j].n,
								  NULL, sam9x7_plls[i][0].hw, i,
								  sam9x7_plls[i][j].c,
								  sam9x7_plls[i][j].l,
								  sam9x7_plls[i][j].f, 0);
				break;

			default:
				continue;
			}

			if (IS_ERR(hw))
				goto err_free;

			sam9x7_plls[i][j].hw = hw;
			if (sam9x7_plls[i][j].eid)
				sam9x7_pmc->chws[sam9x7_plls[i][j].eid] = hw;
		}
	}

	parent_hws[0] = md_slck_hw;
	parent_hws[1] = sam9x7_pmc->chws[PMC_MAIN];
	parent_hws[2] = sam9x7_plls[PLL_ID_PLLA][PLL_COMPID_DIV0].hw;
	parent_hws[3] = sam9x7_plls[PLL_ID_UPLL][PLL_COMPID_DIV0].hw;
	hw = at91_clk_register_master_pres(regmap, "masterck_pres", 4,
					   NULL, parent_hws, &sam9x7_master_layout,
					   &mck_characteristics, &mck_lock);
	if (IS_ERR(hw))
		goto err_free;

	hw = at91_clk_register_master_div(regmap, "masterck_div",
					  NULL, hw, &sam9x7_master_layout,
					  &mck_characteristics, &mck_lock,
					  CLK_SET_RATE_GATE, 0);
	if (IS_ERR(hw))
		goto err_free;

	sam9x7_pmc->chws[PMC_MCK] = hw;

	parent_names[0] = "plla_divpmcck";
	parent_names[1] = "upll_divpmcck";
	parent_names[2] = "main_osc";
	usbck_hw = sam9x60_clk_register_usb(regmap, "usbck", parent_names, 3);
	if (IS_ERR(usbck_hw))
		goto err_free;

	parent_hws[0] = md_slck_hw;
	parent_hws[1] = td_slck_hw;
	parent_hws[2] = sam9x7_pmc->chws[PMC_MAIN];
	parent_hws[3] = sam9x7_pmc->chws[PMC_MCK];
	parent_hws[4] = sam9x7_plls[PLL_ID_PLLA][PLL_COMPID_DIV0].hw;
	parent_hws[5] = sam9x7_plls[PLL_ID_UPLL][PLL_COMPID_DIV0].hw;
	parent_hws[6] = sam9x7_plls[PLL_ID_AUDIO][PLL_COMPID_DIV0].hw;
	for (i = 0; i < 2; i++) {
		char name[6];

		snprintf(name, sizeof(name), "prog%d", i);

		hw = at91_clk_register_programmable(regmap, name,
						    NULL, parent_hws, 7, i,
						    &sam9x7_programmable_layout,
						    NULL);
		if (IS_ERR(hw))
			goto err_free;

		sam9x7_pmc->pchws[i] = hw;
	}

	/* Set systemck parent hws. */
	sam9x7_systemck[0].parent_hw = sam9x7_pmc->chws[PMC_MCK];
	sam9x7_systemck[1].parent_hw = usbck_hw;
	sam9x7_systemck[2].parent_hw = sam9x7_pmc->pchws[0];
	sam9x7_systemck[3].parent_hw = sam9x7_pmc->pchws[1];
	for (i = 0; i < ARRAY_SIZE(sam9x7_systemck); i++) {
		hw = at91_clk_register_system(regmap, sam9x7_systemck[i].n,
					      NULL, sam9x7_systemck[i].parent_hw,
					      sam9x7_systemck[i].id,
					      sam9x7_systemck[i].flags);
		if (IS_ERR(hw))
			goto err_free;

		sam9x7_pmc->shws[sam9x7_systemck[i].id] = hw;
	}

	for (i = 0; i < ARRAY_SIZE(sam9x7_periphck); i++) {
		hw = at91_clk_register_sam9x5_peripheral(regmap, &pmc_pcr_lock,
							 &sam9x7_pcr_layout,
							 sam9x7_periphck[i].n,
							 NULL, sam9x7_pmc->chws[PMC_MCK],
							 sam9x7_periphck[i].id,
							 &range, INT_MIN,
							 sam9x7_periphck[i].f);
		if (IS_ERR(hw))
			goto err_free;

		sam9x7_pmc->phws[sam9x7_periphck[i].id] = hw;
	}

	parent_hws[0] = md_slck_hw;
	parent_hws[1] = td_slck_hw;
	parent_hws[2] = sam9x7_pmc->chws[PMC_MAIN];
	parent_hws[3] = sam9x7_pmc->chws[PMC_MCK];
	for (i = 0; i < ARRAY_SIZE(sam9x7_gck); i++) {
		u8 num_parents = 4 + sam9x7_gck[i].pp_count;
		struct clk_hw *tmp_parent_hws[6];
		u32 *mux_table;

		mux_table = kmalloc_array(num_parents, sizeof(*mux_table),
					  GFP_KERNEL);
		if (!mux_table)
			goto err_free;

		PMC_INIT_TABLE(mux_table, 4);
		PMC_FILL_TABLE(&mux_table[4], sam9x7_gck[i].pp_mux_table,
			       sam9x7_gck[i].pp_count);

		for (j = 0; j < sam9x7_gck[i].pp_count; j++) {
			u8 pll_id = sam9x7_gck[i].pp[j].pll_id;
			u8 pll_compid = sam9x7_gck[i].pp[j].pll_compid;

			tmp_parent_hws[j] = sam9x7_plls[pll_id][pll_compid].hw;
		}

		PMC_FILL_TABLE(&parent_hws[4], tmp_parent_hws,
			       sam9x7_gck[i].pp_count);

		hw = at91_clk_register_generated(regmap, &pmc_pcr_lock,
						 &sam9x7_pcr_layout,
						 sam9x7_gck[i].n,
						 NULL, parent_hws, mux_table,
						 num_parents,
						 sam9x7_gck[i].id,
						 &sam9x7_gck[i].r,
						 sam9x7_gck[i].pp_chg_id);
		if (IS_ERR(hw))
			goto err_free;

		sam9x7_pmc->ghws[sam9x7_gck[i].id] = hw;
		clk_mux_buffer[clk_mux_buffer_size++] = mux_table;
	}

	of_clk_add_hw_provider(np, of_clk_hw_pmc_get, sam9x7_pmc);
	kfree(clk_mux_buffer);

	return;

err_free:
	if (clk_mux_buffer) {
		for (i = 0; i < clk_mux_buffer_size; i++)
			kfree(clk_mux_buffer[i]);
		kfree(clk_mux_buffer);
	}
	kfree(sam9x7_pmc);
}

/* Some clks are used for a clocksource */
CLK_OF_DECLARE(sam9x7_pmc, "microchip,sam9x7-pmc", sam9x7_pmc_setup);
