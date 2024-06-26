/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Synopsys, Inc., Intel Corporation
 */

#ifndef __INTEL_PLL_ALGORITHM_H__
#define __INTEL_PLL_ALGORITHM_H__

#include <linux/types.h>

struct drm_i915_private;
struct intel_atomic_state;
struct intel_c10pll_state;
struct intel_crtc;
struct intel_crtc_state;
struct intel_encoder;
struct intel_mpllb_state;
enum phy;

struct pll_output_params {
	u32 ssc_up_spread;
	u32 mpll_div5_en;
	u32 hdmi_div;
	u32 ana_cp_int;
	u32 ana_cp_prop;
	u32 refclk_postscalar;
	u32 tx_clk_div;
	u32 fracn_quot;
	u32 fracn_rem;
	u32 fracn_den;
	u32 fracn_en;
	u32 pmix_en;
	u32 multiplier;
	int mpll_ana_v2i;
	int ana_freq_vco;
};

int intel_snps_phy_compute_hdmi_tmds_pll(u64 pixel_clock, struct intel_mpllb_state *pll_state);
int intel_c10_phy_compute_hdmi_tmds_pll(u64 pixel_clock, struct intel_c10pll_state *pll_state);

#endif /* __INTEL_PLL_ALGORITHM_H__ */
