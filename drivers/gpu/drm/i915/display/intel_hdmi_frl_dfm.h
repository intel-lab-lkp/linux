/* SPDX-License-Identifier: MIT
 * Copyright © 2022 Intel Corp
 */

#ifndef __INTEL_HDMI_FRL_DFM_H__
#define __INTEL_HDMI_FRL_DFM_H__

#include <linux/types.h>

/* All the input config needed to compute DFM requirements */
struct intel_hdmi_frl_dfm_input_config {
	/*
	 * Pixel clock rate kHz, when FVA is
	 * enabled this rate is the rate after adjustment
	 */
	u32 pixel_clock_nominal_khz;

	/* active pixels per line */
	u32 hactive;

	/* Blanking pixels per line */
	u32 hblank;

	/* Bits per component */
	u32 bpc;

	/* Pixel encoding */
	u32 color_format;

	/* FRL bit rate in kbps */
	u32 bit_rate_kbps;

	/* FRL lanes */
	u32 lanes;

	/* Number of audio channels */
	u32 audio_channels;

	/* Audio rate in Hz */
	u32 audio_hz;

	/* Selected bpp target value */
	u32 target_bpp_16;

	/*
	 * Number of horizontal pixels in a slice.
	 * Equivalent to PPS parameter slice_width
	 */
	u32 slice_width;
};

/* Computed dfm parameters for FRL */
struct intel_hdmi_frl_dfm_params {
	/*
	 * Link overhead in percentage
	 * multiplied by 1000 (efficiency multiplier)
	 */
	u32 overhead_max;

	/* Maximum pixel rate in kHz */
	u32 pixel_clock_max_khz;

	/* Minimum video line period in nano sec */
	u32 line_time_ns;

	/* worst case slow frl character rate in kbps */
	u32 char_rate_min_kbps;

	/* minimum total frl charecters per line period */
	u32 cfrl_line;

	/* Average tribyte rate in khz */
	u32 ftb_avg_k;

	/* Audio characteristics */

	/*  number of audio packets needed during hblank */
	u32 num_audio_pkts_line;

	/*
	 *  Minimum required hblank assuming no control period
	 *  RC compression
	 */
	u32 hblank_audio_min;

	/* Number of tribytes required to carry active video */
	u32 tb_active;

	/* Total available tribytes during the blanking period */
	u32 tb_blank;

	/*
	 * Number of tribytes required to be transmitted during
	 * the hblank period
	 */
	u32 tb_borrowed;

	/* DSC frl characteristics */

	/* Tribytes required to carry the target bpp */
	u32 hcactive_target;

	/* tribytes available during blanking with target bpp */
	u32 hcblank_target;
};

/* FRL DFM structure to hold data involved in DFM computation */
struct intel_hdmi_frl_dfm {
	struct intel_hdmi_frl_dfm_input_config config;
	struct intel_hdmi_frl_dfm_params params;
};

bool intel_hdmi_frl_dfm_nondsc_requirement_met(struct intel_hdmi_frl_dfm *frl_dfm);

bool
intel_hdmi_frl_dfm_dsc_requirement_met(struct intel_hdmi_frl_dfm *frl_dfm);

#endif
