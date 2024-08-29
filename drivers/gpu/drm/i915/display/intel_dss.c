// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_reg_defs.h"
#include "intel_de.h"
#include "intel_display_limits.h"
#include "intel_display_types.h"
#include "intel_dsi.h"
#include "intel_dss.h"
#include "intel_dss_regs.h"
#include "intel_vdsc.h"

/*
 * Splitter enable for eDP MSO is limited to certain pipes, on certain
 * platforms.
 */
u8 intel_dss_mso_pipe_mask(struct intel_display *display)
{
	struct drm_i915_private *i915 = to_i915(display->drm);

	if (DISPLAY_VER(display) > 20)
		return ~0;
	else if (IS_ALDERLAKE_P(i915))
		return BIT(PIPE_A) | BIT(PIPE_B);
	else
		return BIT(PIPE_A);
}

void intel_dss_mso_get_config(struct intel_encoder *encoder,
			      struct intel_crtc_state *pipe_config)
{
	struct intel_display *display = to_intel_display(pipe_config);
	struct intel_crtc *crtc = to_intel_crtc(pipe_config->uapi.crtc);
	enum pipe pipe = crtc->pipe;
	u32 dss1;

	if (!HAS_MSO(display))
		return;

	dss1 = intel_de_read(display, ICL_PIPE_DSS_CTL1(pipe));

	pipe_config->splitter.enable = dss1 & SPLITTER_ENABLE;
	if (!pipe_config->splitter.enable)
		return;

	if (drm_WARN_ON(display->drm, !(intel_dss_mso_pipe_mask(display) & BIT(pipe)))) {
		pipe_config->splitter.enable = false;
		return;
	}

	switch (dss1 & SPLITTER_CONFIGURATION_MASK) {
	default:
		drm_WARN(display->drm, true,
			 "Invalid splitter configuration, dss1=0x%08x\n", dss1);
		fallthrough;
	case SPLITTER_CONFIGURATION_2_SEGMENT:
		pipe_config->splitter.link_count = 2;
		break;
	case SPLITTER_CONFIGURATION_4_SEGMENT:
		pipe_config->splitter.link_count = 4;
		break;
	}

	pipe_config->splitter.pixel_overlap = REG_FIELD_GET(OVERLAP_PIXELS_MASK, dss1);
}

void intel_dss_mso_configure(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	enum pipe pipe = crtc->pipe;
	u32 dss1 = 0;

	if (!HAS_MSO(display))
		return;

	if (crtc_state->splitter.enable) {
		dss1 |= SPLITTER_ENABLE;
		dss1 |= OVERLAP_PIXELS(crtc_state->splitter.pixel_overlap);
		if (crtc_state->splitter.link_count == 2)
			dss1 |= SPLITTER_CONFIGURATION_2_SEGMENT;
		else
			dss1 |= SPLITTER_CONFIGURATION_4_SEGMENT;
	}

	intel_de_rmw(display, ICL_PIPE_DSS_CTL1(pipe),
		     SPLITTER_ENABLE | SPLITTER_CONFIGURATION_MASK |
		     OVERLAP_PIXELS_MASK, dss1);
}

void intel_dss_dsi_dual_link_mode_configure(struct intel_encoder *encoder,
					    const struct intel_crtc_state *pipe_config,
					    u8 dual_link,
					    u8 pixel_overlap)
{
	struct intel_display *display = to_intel_display(encoder);
	i915_reg_t dss_ctl1_reg, dss_ctl2_reg;
	u32 dss_ctl1;

	if (DISPLAY_VER(display) >= 12) {
		struct intel_crtc *crtc = to_intel_crtc(pipe_config->uapi.crtc);

		dss_ctl1_reg = ICL_PIPE_DSS_CTL1(crtc->pipe);
		dss_ctl2_reg = ICL_PIPE_DSS_CTL2(crtc->pipe);
	} else {
		dss_ctl1_reg = DSS_CTL1;
		dss_ctl2_reg = DSS_CTL2;
	}

	dss_ctl1 = intel_de_read(display, dss_ctl1_reg);
	dss_ctl1 |= SPLITTER_ENABLE;
	dss_ctl1 &= ~OVERLAP_PIXELS_MASK;
	dss_ctl1 |= OVERLAP_PIXELS(pixel_overlap);

	if (dual_link == DSI_DUAL_LINK_FRONT_BACK) {
		const struct drm_display_mode *adjusted_mode =
					&pipe_config->hw.adjusted_mode;
		u16 hactive = adjusted_mode->crtc_hdisplay;
		u16 dl_buffer_depth;

		dss_ctl1 &= ~DUAL_LINK_MODE_INTERLEAVE;
		dl_buffer_depth = hactive / 2 + pixel_overlap;

		if (dl_buffer_depth > MAX_DL_BUFFER_TARGET_DEPTH)
			drm_err(display->drm,
				"DL buffer depth exceed max value\n");

		dss_ctl1 &= ~LEFT_DL_BUF_TARGET_DEPTH_MASK;
		dss_ctl1 |= LEFT_DL_BUF_TARGET_DEPTH(dl_buffer_depth);
		intel_de_rmw(display, dss_ctl2_reg, RIGHT_DL_BUF_TARGET_DEPTH_MASK,
			     RIGHT_DL_BUF_TARGET_DEPTH(dl_buffer_depth));
	} else {
		/* Interleave */
		dss_ctl1 |= DUAL_LINK_MODE_INTERLEAVE;
	}

	intel_de_write(display, dss_ctl1_reg, dss_ctl1);
}

static i915_reg_t dss_ctl1_reg(struct intel_crtc *crtc, enum transcoder cpu_transcoder)
{
	return intel_dsc_is_dsc_pipe(crtc, cpu_transcoder) ?
		ICL_PIPE_DSS_CTL1(crtc->pipe) : DSS_CTL1;
}

static i915_reg_t dss_ctl2_reg(struct intel_crtc *crtc, enum transcoder cpu_transcoder)
{
	return intel_dsc_is_dsc_pipe(crtc, cpu_transcoder) ?
		ICL_PIPE_DSS_CTL2(crtc->pipe) : DSS_CTL2;
}

void intel_dss_reset(const struct intel_crtc_state *old_crtc_state)
{
	struct intel_display *display = to_intel_display(old_crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(old_crtc_state->uapi.crtc);

	intel_de_write(display, dss_ctl1_reg(crtc, old_crtc_state->cpu_transcoder), 0);
	intel_de_write(display, dss_ctl2_reg(crtc, old_crtc_state->cpu_transcoder), 0);
}

void intel_dss_enable_uncompressed_joiner(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 dss_ctl1_val = 0;

	if (crtc_state->joiner_pipes && !crtc_state->dsc.compression_enable) {
		if (intel_crtc_is_joiner_secondary(crtc_state))
			dss_ctl1_val |= UNCOMPRESSED_JOINER_SECONDARY;
		else
			dss_ctl1_val |= UNCOMPRESSED_JOINER_PRIMARY;

		intel_de_write(display,
			       dss_ctl1_reg(crtc, crtc_state->cpu_transcoder),
			       dss_ctl1_val);
	}
}

void intel_dss_enable_compressed_joiner(const struct intel_crtc_state *crtc_state,
					int vdsc_instances_per_pipe)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 dss_ctl1_val = 0;
	u32 dss_ctl2_val = 0;

	dss_ctl2_val |= LEFT_BRANCH_VDSC_ENABLE;
	if (vdsc_instances_per_pipe > 1) {
		dss_ctl2_val |= RIGHT_BRANCH_VDSC_ENABLE;
		dss_ctl1_val |= JOINER_ENABLE;
	}
	if (crtc_state->joiner_pipes) {
		dss_ctl1_val |= BIG_JOINER_ENABLE;
		if (!intel_crtc_is_joiner_secondary(crtc_state))
			dss_ctl1_val |= PRIMARY_BIG_JOINER_ENABLE;
	}
	intel_de_write(display, dss_ctl1_reg(crtc, crtc_state->cpu_transcoder), dss_ctl1_val);
	intel_de_write(display, dss_ctl2_reg(crtc, crtc_state->cpu_transcoder), dss_ctl2_val);
}

void intel_dss_dsc_get_config(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	struct intel_crtc *crtc = to_intel_crtc(crtc_state->uapi.crtc);
	u32 dss_ctl1, dss_ctl2;

	dss_ctl1 = intel_de_read(display, dss_ctl1_reg(crtc, crtc_state->cpu_transcoder));
	dss_ctl2 = intel_de_read(display, dss_ctl2_reg(crtc, crtc_state->cpu_transcoder));

	crtc_state->dsc.compression_enable = dss_ctl2 & LEFT_BRANCH_VDSC_ENABLE;
	if (!crtc_state->dsc.compression_enable)
		return;

	crtc_state->dsc.dsc_split = (dss_ctl2 & RIGHT_BRANCH_VDSC_ENABLE) &&
				    (dss_ctl1 & JOINER_ENABLE);
}
