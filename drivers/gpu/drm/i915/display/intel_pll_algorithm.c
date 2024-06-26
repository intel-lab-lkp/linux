// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Synopsys, Inc., Intel Corporation
 */

#include <linux/math.h>
#include "i915_reg.h"
#include "intel_ddi.h"
#include "intel_ddi_buf_trans.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_snps_phy.h"
#include "intel_snps_phy_regs.h"
#include "intel_pll_algorithm.h"

#define INTEL_SNPS_PHY_HDMI_4999MHZ 4999999900ull
#define INTEL_SNPS_PHY_HDMI_16GHZ 16000000000ull
#define INTEL_SNPS_PHY_HDMI_9999MHZ (2 * INTEL_SNPS_PHY_HDMI_4999MHZ)

#define CURVE0_MULTIPLIER 1000000000
#define CURVE1_MULTIPLIER 100
#define CURVE2_MULTIPLIER 1000000000000

static int64_t interp(s64 x, s64 x1, s64 x2, s64 y1, s64 y2)
{
	s64 dydx;

	dydx = DIV_ROUND_UP((y2 - y1) * 100000, (x2 - x1));

	return (y1 + DIV_ROUND_UP(dydx * (x - x1), 100000));
}

static void get_ana_cp_int_prop(u32 vco_clk,
				u32 refclk_postscalar,
				int mpll_ana_v2i,
				int c, int a,
				const u64 curve_freq_hz[2][8],
				const u64 curve_0[2][8],
				const u64 curve_1[2][8],
				const u64 curve_2[2][8],
				u32 *ana_cp_int,
				u32 *ana_cp_prop)
{
	u64 vco_div_refclk_float;
	u64 o_397ced90;
	u64 o_20c634d6;
	u64 o_20c634d4;
	u64 o_72019306;
	u64 o_6593e82b;
	u64 o_5cefc329;
	u64 o_49960328;
	u64 o_544adb37;
	u64 o_4ef74e66;
	u32 ana_cp_int_temp;
	u64 temp1, temp2, temp3, temp4;

	vco_div_refclk_float = vco_clk * (1000000000000 / refclk_postscalar);
	o_397ced90 = interp(vco_clk, curve_freq_hz[c][a], curve_freq_hz[c][a + 1],
			    curve_0[c][a], curve_0[c][a + 1]);

	o_20c634d6 = interp(vco_clk, curve_freq_hz[c][a], curve_freq_hz[c][a + 1],
			    curve_2[c][a], curve_2[c][a + 1]);

	o_20c634d4 = interp(vco_clk, curve_freq_hz[c][a], curve_freq_hz[c][a + 1],
			    curve_1[c][a], curve_1[c][a + 1]);

	o_20c634d4 /= CURVE1_MULTIPLIER;

	temp1 = o_20c634d6 * (4 - mpll_ana_v2i);

	o_72019306 = temp1 / 16000;
	o_6593e82b = temp1 / 160;

	temp2 = ((112008301 * (vco_div_refclk_float / 100000)));
	o_5cefc329 = CURVE2_MULTIPLIER * (temp2 / (o_397ced90 * (o_20c634d4 / CURVE0_MULTIPLIER)));

	ana_cp_int_temp = min(DIV_ROUND_CLOSEST(o_5cefc329 / o_72019306, CURVE2_MULTIPLIER), 127);
	ana_cp_int_temp = max(1, ana_cp_int_temp);

	*ana_cp_int = ana_cp_int_temp;

	o_49960328 = o_72019306 * ana_cp_int_temp;

	temp3 = o_20c634d4 * (o_49960328 * o_397ced90 / CURVE0_MULTIPLIER);
	o_544adb37 = int_sqrt(DIV_ROUND_UP(temp3, vco_div_refclk_float) * (1000000000000 / 55));

	temp4 = DIV_ROUND_UP(vco_div_refclk_float, 1000000);
	o_4ef74e66 = (1460281 * DIV_ROUND_UP(o_544adb37 * temp4, o_20c634d4));

	*ana_cp_prop = max(1, min(DIV_ROUND_UP(o_4ef74e66, o_6593e82b), 127));
}

static int _intel_phy_compute_hdmi_tmds_pll(u64 pixel_clock, u32 refclk,
					    u32 ref_range,
					    u32 ana_cp_int_gs,
					    u32 ana_cp_prop_gs,
					    const u64 curve_freq_hz[2][8],
					    const u64 curve_0[2][8],
					    const u64 curve_1[2][8],
					    const u64 curve_2[2][8],
					    u32 prescaler_divider,
					    struct pll_output_params *pll_params)
{
	/*datarate 10khz */
	u64 datarate = pixel_clock * 10000;
	u32 ssc_up_spread = 1;
	u32 mpll_div5_en = 1;
	u32 hdmi_div = 1;
	u32 ana_cp_int;
	u32 ana_cp_prop;
	u32 refclk_postscalar = refclk >> prescaler_divider;
	u32 tx_clk_div;
	u64 vco_clk;
	u32 vco_div_refclk_integer;
	u32 vco_div_refclk_fracn;
	u32 fracn_quot;
	u32 fracn_rem;
	u32 fracn_den;
	u32 fracn_en;
	u32 pmix_en;
	u32 multiplier;
	int mpll_ana_v2i;
	int ana_freq_vco;
	int c, a, j;

	if (pixel_clock < 25175 || pixel_clock > 600000)
		return -EINVAL;

	/* Select appropriate v2i point */
	if (datarate <= INTEL_SNPS_PHY_HDMI_9999MHZ) {
		mpll_ana_v2i = 2;
		tx_clk_div = ilog2(INTEL_SNPS_PHY_HDMI_9999MHZ / datarate);
	} else {
		mpll_ana_v2i = 3;
		tx_clk_div = ilog2(INTEL_SNPS_PHY_HDMI_16GHZ / datarate);
	}
	vco_clk = (datarate << tx_clk_div) >> 1;

	/* Highly accurate division, calculate fraction to 32 bits of precision */
	vco_div_refclk_integer = vco_clk / refclk_postscalar;
	vco_div_refclk_fracn = ((vco_clk % refclk_postscalar) << 32) / refclk_postscalar;
	fracn_quot = vco_div_refclk_fracn >> 16;
	fracn_rem = vco_div_refclk_fracn & 0xffff;
	fracn_rem = fracn_rem - (fracn_rem >> 15);
	fracn_den = 0xffff;
	fracn_en = (fracn_quot != 0 || fracn_rem != 0) ? 1 : 0;
	pmix_en = fracn_en;
	multiplier = (vco_div_refclk_integer - 16) * 2;
	/* Curve selection for ana_cp_* calculations. One curve hardcoded per v2i range */
	c = mpll_ana_v2i - 2;

	/* Find the right segment of the table */
	for (j = 0; j < 8; j += 2) {
		if (vco_clk <= curve_freq_hz[c][j + 1]) {
			a = j;
			ana_freq_vco = 3 - (a >> 1);
			break;
		}
	}

	get_ana_cp_int_prop(vco_clk, refclk_postscalar, mpll_ana_v2i, c, a,
			    curve_freq_hz, curve_0, curve_1, curve_2,
			    &ana_cp_int, &ana_cp_prop);

	pll_params->ssc_up_spread = ssc_up_spread;
	pll_params->mpll_div5_en = mpll_div5_en;
	pll_params->hdmi_div = hdmi_div;
	pll_params->ana_cp_int = ana_cp_int;
	pll_params->refclk_postscalar = refclk_postscalar;
	pll_params->tx_clk_div = tx_clk_div;
	pll_params->fracn_quot = fracn_quot;
	pll_params->fracn_rem = fracn_rem;
	pll_params->fracn_den = fracn_den;
	pll_params->fracn_en = fracn_en;
	pll_params->pmix_en = pmix_en;
	pll_params->multiplier = multiplier;
	pll_params->ana_cp_prop = ana_cp_prop;
	pll_params->mpll_ana_v2i = mpll_ana_v2i;
	pll_params->ana_freq_vco = ana_freq_vco;

	return 0;
}

int intel_snps_phy_compute_hdmi_tmds_pll(u64 pixel_clock, struct intel_mpllb_state *pll_state)
{
	struct pll_output_params pll_params;
	u32 refclk = 100000000;
	u32 prescaler_divider = 1;
	u32 ref_range = 3;
	u32 ana_cp_int_gs = 64;
	u32 ana_cp_prop_gs = 124;
	int ret;
	/* x axis frequencies. One curve in each array per v2i point */
	const u64 dg2_curve_freq_hz[2][8] = {
		{2500000000, 3000000000, 3000000000, 3500000000, 3500000000, 4000000000, 4000000000, 5000000000},
		{4000000000, 4600000000, 4601000000, 5400000000, 5401000000, 6600000000, 6601000000, 8001000000}};

	/* y axis heights multiplied with 1000000000 */
	const u64 dg2_curve_0[2][8] = {
		{34149871, 39803269, 36034544, 40601014, 35646940, 40016109, 35127987, 41889522},
		{70000000, 78770454, 70451838, 80427119, 70991400, 84230173, 72945921, 87064218}};

	/* Multiplied with 100 */
	const u64 dg2_curve_1[2][8] = {
		{85177000000000, 79385227160000, 95672603580000, 88857207160000, 109379790900000, 103528193900000, 131941242400000, 117279000000000},
		{60255000000000, 55569000000000, 72036000000000, 69509000000000,  81785000000000, 731030000000000, 96591000000000, 69077000000000}};

	/* Multiplied with 1000000000000 */
	const u64 dg2_curve_2[2][8] = {
		{2186930000, 2835287134, 2395395343, 2932270687, 2351887545, 2861031697, 2294149152, 3091730000},
		{4560000000, 5570000000, 4610000000, 5770000000, 4670000000, 6240000000, 4890000000, 6600000000}
	};

	ret = _intel_phy_compute_hdmi_tmds_pll(pixel_clock, refclk, ref_range,
					       ana_cp_int_gs, ana_cp_prop_gs,
					       dg2_curve_freq_hz, dg2_curve_0,
					       dg2_curve_1, dg2_curve_2, prescaler_divider,
					       &pll_params);

	if (ret)
		return ret;

	pll_state->clock = pixel_clock;
	pll_state->ref_control =
		REG_FIELD_PREP(SNPS_PHY_REF_CONTROL_REF_RANGE, ref_range);
	pll_state->mpllb_cp =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT, pll_params.ana_cp_int) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP, pll_params.ana_cp_prop) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_INT_GS, ana_cp_int_gs) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_CP_PROP_GS, ana_cp_prop_gs);
	pll_state->mpllb_div =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_DIV5_CLK_EN, pll_params.mpll_div5_en) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_TX_CLK_DIV, pll_params.tx_clk_div) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_PMIX_EN, pll_params.pmix_en) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_V2I, pll_params.mpll_ana_v2i) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FREQ_VCO, pll_params.ana_freq_vco);
	pll_state->mpllb_div2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_REF_CLK_DIV, prescaler_divider) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_MULTIPLIER, pll_params.multiplier) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_HDMI_DIV, pll_params.hdmi_div);
	pll_state->mpllb_fracn1 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_CGG_UPDATE_EN, 1) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_EN, pll_params.fracn_en) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_DEN, pll_params.fracn_den);
	pll_state->mpllb_fracn2 =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_QUOT, pll_params.fracn_quot) |
		REG_FIELD_PREP(SNPS_PHY_MPLLB_FRACN_REM, pll_params.fracn_rem);
	pll_state->mpllb_sscen =
		REG_FIELD_PREP(SNPS_PHY_MPLLB_SSC_UP_SPREAD, pll_params.ssc_up_spread);

	return 0;
}
