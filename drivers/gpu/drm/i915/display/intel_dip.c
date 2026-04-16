// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 *
 */

#include <drm/drm_print.h>

#include "intel_de.h"
#include "intel_dip.h"
#include "intel_dip_regs.h"
#include "intel_display_types.h"
#include "intel_dp.h"

u16 intel_dip_read_emp_as_sdp_tl(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 val;

	if (!HAS_EMP_AS_SDP_TL(display))
		return 0;

	val = intel_de_read(display, EMP_AS_SDP_TL(display, cpu_transcoder));
	return REG_FIELD_GET(EMP_AS_SDP_DB_TL_MASK, val);
}

void intel_dip_write_emp_as_sdp_tl(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;

	if (!HAS_EMP_AS_SDP_TL(display))
		return;
	/*
	 * Since currently we support VRR only for DP/eDP, so this is programmed
	 * only for Adaptive Sync SDP.
	 */
	if (intel_crtc_has_dp_encoder(crtc_state))
		intel_de_write(display,
			       EMP_AS_SDP_TL(display, cpu_transcoder),
			       EMP_AS_SDP_DB_TL(crtc_state->dip.emp_as_sdp_tl));
}

void intel_dip_sdp_tl_compute_config_late(struct intel_crtc_state *crtc_state)
{
	crtc_state->dip.emp_as_sdp_tl = intel_dp_get_as_sdp_transmission_line(crtc_state);
}

void intel_dip_sdp_transmission_line_get_config(struct intel_crtc_state *crtc_state)
{
	crtc_state->dip.emp_as_sdp_tl = intel_dip_read_emp_as_sdp_tl(crtc_state);
}

static int intel_dip_sdp_tl_to_stagger(const struct intel_crtc_state *crtc_state,
				       u16 sdp_transmission_line)
{
	return sdp_transmission_line - crtc_state->dip.cmn_sdp_tl;
}

void intel_dip_cmn_sdp_transmission_line_enable(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	int gmp_stagger;
	int pps_stagger;
	int vsc_ext_stagger;

	if (!crtc_state->dip.cmn_sdp_tl)
		return;

	gmp_stagger = intel_dip_sdp_tl_to_stagger(crtc_state,
						  crtc_state->dip.gmp_sdp_tl);

	pps_stagger = intel_dip_sdp_tl_to_stagger(crtc_state,
						  crtc_state->dip.pps_sdp_tl);

	vsc_ext_stagger = intel_dip_sdp_tl_to_stagger(crtc_state,
						      crtc_state->dip.vsc_ext_sdp_tl);

	if (drm_WARN_ON(display->drm, gmp_stagger < 0))
		return;
	if (drm_WARN_ON(display->drm, pps_stagger < 0))
		return;
	if (drm_WARN_ON(display->drm, vsc_ext_stagger < 0))
		return;

	intel_de_write(display, CMN_SDP_TL_STGR_CTL(display, cpu_transcoder),
		       GMP_STAGGER(gmp_stagger) |
		       PPS_STAGGER(pps_stagger) |
		       VSC_EXT_STAGGER(vsc_ext_stagger));

	intel_de_write(display, CMN_SDP_TL(display, cpu_transcoder),
		       TRANSMISSION_LINE_ENABLE |
		       BASE_TRANSMISSION_LINE(crtc_state->dip.cmn_sdp_tl));
}

void intel_dip_cmn_sdp_transmission_line_disable(const struct intel_crtc_state *old_crtc_state)
{
	struct intel_display *display = to_intel_display(old_crtc_state);
	enum transcoder cpu_transcoder = old_crtc_state->cpu_transcoder;

	if (!old_crtc_state->dip.cmn_sdp_tl)
		return;

	intel_de_write(display, CMN_SDP_TL(display, cpu_transcoder), 0);
}
