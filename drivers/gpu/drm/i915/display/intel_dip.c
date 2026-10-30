// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 *
 */

#include <linux/minmax.h>

#include <drm/drm_print.h>

#include "intel_de.h"
#include "intel_dip.h"
#include "intel_dip_regs.h"
#include "intel_display_regs.h"
#include "intel_display_types.h"
#include "intel_hdmi.h"

static int intel_dip_get_as_sdp_transmission_line(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);

	if (!HAS_EMP_AS_SDP_TL(display))
		return 0;

	/*
	 * HDMI 2.1 expects the EMPs (such as the VTEM used for VRR) within
	 * the first half of V. Blank, so send them on the 2nd line of the
	 * V. Blank rather than at the start of the VSYNC pulse. The line
	 * must also stay within the SCL + guardband region.
	 */
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI) &&
	    crtc_state->vrr.in_range) {
		const struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;
		int max_tl = crtc_state->set_context_latency +
			crtc_state->vrr.guardband - 1;
		int tl = mode->crtc_vtotal - mode->crtc_vdisplay - 1;

		if (max_tl > 0)
			tl = min(tl, max_tl);

		return tl;
	}

	/*
	 * EMP_AS_SDP_TL defines the T1 position as the default AS SDP
	 * Transmission Line, which corresponds to the start of the
	 * VSYNC pulse.
	 *
	 * Use the T1 position for now.
	 */
	return crtc_state->vrr.vsync_start;
}

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

	intel_de_write(display,
		       EMP_AS_SDP_TL(display, cpu_transcoder),
		       EMP_AS_SDP_DB_TL(crtc_state->dip.emp_as_sdp_tl));
}

/* VTEM: 7 byte EM Data Set header (PB0-PB6) + 4 byte data set (MD0-MD3) */
#define VTEM_DATA_SET_LENGTH	4
#define VTEM_EMP_SIZE		(7 + VTEM_DATA_SET_LENGTH)
/* Payload bytes carried by one EMP data island packet */
#define HDMI_EMP_PACKET_BODY	28

/*
 * Tell an HDMI 2.1 sink whether VRR is active with a Class 0 (Gaming-VRR)
 * Video Timing Extended Metadata (VTEM) packet. It is a periodic
 * pseudo-static EM Data Set: the hardware sends it every frame until it is
 * reprogrammed. On disable, a last VTEM with VRR_EN=0 and END set is sent,
 * after which the hardware stops sending it.
 */
void intel_dip_write_hdmi_vtem(const struct intel_crtc_state *crtc_state,
			       bool enable)
{
	struct intel_display *display = to_intel_display(crtc_state);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	const struct drm_display_mode *mode = &crtc_state->hw.adjusted_mode;
	u8 vic = crtc_state->infoframes.avi.avi.video_code;
	int base_rr = drm_mode_vrefresh(mode);
	u8 pb[HDMI_EMP_PACKET_BODY] = {};
	int i;

	if (DISPLAY_VER(display) < 20 ||
	    !intel_crtc_has_type(crtc_state, INTEL_OUTPUT_HDMI) ||
	    !crtc_state->vrr.in_range)
		return;

	/* PB0-PB6: EM Data Set header (Sync = 0, DS_Type = pseudo-static) */
	pb[0] = BIT(7) |			/* NEW */
		BIT(2) |			/* VFR */
		(enable ? 0 : BIT(6));		/* END */
	pb[2] = 1;				/* Organization_ID: HDMI */
	pb[4] = 1;				/* Data_Set_Tag: VTEM */
	pb[6] = VTEM_DATA_SET_LENGTH;		/* Data_Set_Length */

	/* PB7-PB10: MD0-MD3 (M_CONST = 0, FVA_Factor_M1 = 0, MD2 bit 2 reserved) */
	if (enable) {
		pb[7] = BIT(0);			/* VRR_EN */
		/* Base_Vfront and Base_Refresh_Rate are only sent without a VIC */
		if (!vic) {
			pb[8] = min(mode->crtc_vsync_start - mode->crtc_vdisplay, 255);
			pb[9] = (base_rr >> 8) & 0x3;
			pb[10] = base_rr & 0xff;
		}
	}

	drm_dbg_kms(display->drm,
		    "[TRANSCODER %s] VTEM: VRR_EN=%d VIC=%u Base_Vfront=%u Base_RR=%u\n",
		    transcoder_name(cpu_transcoder), enable, vic, pb[8],
		    pb[9] << 8 | pb[10]);

	/*
	 * HDMI_EMP_CTL first, then the payload dwords (PB0 in bits 7:0), then
	 * the header, whose write clock crosses the whole packet. HB0 must be
	 * set to the EMP packet type, the hardware doesn't fill it in.
	 */
	intel_de_write(display, HDMI_EMP_CTL(display, cpu_transcoder),
		       HDMI_EMP_CTL_TYPE_VTEM);
	for (i = 0; i < ARRAY_SIZE(pb); i += 4)
		intel_de_write(display, HDMI_EMP_DATA(display, cpu_transcoder),
			       pb[i] | pb[i + 1] << 8 | pb[i + 2] << 16 |
			       pb[i + 3] << 24);
	intel_de_write(display, HDMI_EMP_HEADER(display, cpu_transcoder),
		       HDMI_EMP_HEADER_HB0_EMP |
		       HDMI_EMP_HEADER_DS_TYPE_PSEUDO_STATIC |
		       (enable ? 0 : HDMI_EMP_HEADER_END) |
		       HDMI_EMP_HEADER_NUM_PACKETS(DIV_ROUND_UP(VTEM_EMP_SIZE,
								HDMI_EMP_PACKET_BODY)));
}

static int intel_dip_sdp_stagger_to_tl(struct intel_crtc_state *crtc_state,
				       int stagger)
{
	return crtc_state->dip.cmn_sdp_tl + stagger;
}

static
void intel_dip_cmn_sdp_tl_compute_config_late(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	bool as_sdp;

	if (!HAS_COMMON_SDP_TL(display))
		return;

	as_sdp = crtc_state->infoframes.enable &
		 intel_hdmi_infoframe_enable(DP_SDP_ADAPTIVE_SYNC);
	/*
	 * When AS SDP is enabled :
	 *  - The common SDP Transmission Line matches the EMP SDP Transmission Line.
	 *
	 * When AS SDP is disabled:
	 *  - Bspec mentions the positions as lines of delayed vblank.
	 *  - Guardband = 1st line of delayed vblank
	 *  - Common SDP Transmission line is set to 2nd line of delayed vblank.
	 */

	if (as_sdp)
		crtc_state->dip.cmn_sdp_tl = crtc_state->dip.emp_as_sdp_tl;
	else
		crtc_state->dip.cmn_sdp_tl = crtc_state->vrr.guardband - 1;

	if (drm_WARN_ON(display->drm,
			crtc_state->dip.cmn_sdp_tl >=
			crtc_state->vrr.guardband + crtc_state->set_context_latency))
		return;

	/*
	 * Currently we are programming the default stagger values, but these
	 * can be optimized if required, based on number of SDPs enabled.
	 *
	 * Default values of the Transmission lines for SDPs other than AS SDP:
	 * VSC : CMN SDP Transmission line
	 * GMP : CMN SDP Transmission line
	 * PPS : CMN SDP Transmission line + 1
	 * VSC_EXT: CMN SDP Transmission line + 2
	 */
	crtc_state->dip.vsc_sdp_tl = crtc_state->dip.cmn_sdp_tl;
	crtc_state->dip.gmp_sdp_tl =
		intel_dip_sdp_stagger_to_tl(crtc_state, GMP_STAGGER_DEFAULT);
	crtc_state->dip.pps_sdp_tl =
		intel_dip_sdp_stagger_to_tl(crtc_state, PPS_STAGGER_DEFAULT);
	crtc_state->dip.vsc_ext_sdp_tl =
		intel_dip_sdp_stagger_to_tl(crtc_state, VSC_EXT_STAGGER_DEFAULT);
}

void intel_dip_sdp_tl_compute_config_late(struct intel_crtc_state *crtc_state)
{
	crtc_state->dip.emp_as_sdp_tl = intel_dip_get_as_sdp_transmission_line(crtc_state);

	intel_dip_cmn_sdp_tl_compute_config_late(crtc_state);
}

static
void intel_dip_cmn_sdp_transmission_line_get_config(struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u16 vsc_ext_stagger, pps_stagger, gmp_stagger;
	u32 val;

	if (!HAS_COMMON_SDP_TL(display))
		return;

	val = intel_de_read(display, CMN_SDP_TL(display, cpu_transcoder));

	if (!(val & TRANSMISSION_LINE_ENABLE))
		return;

	crtc_state->dip.cmn_sdp_tl = REG_FIELD_GET(BASE_TRANSMISSION_LINE_MASK, val);

	/* SDP VSC uses same transmission line as CMN base transmission line */
	crtc_state->dip.vsc_sdp_tl = crtc_state->dip.cmn_sdp_tl;

	val = intel_de_read(display, CMN_SDP_TL_STGR_CTL(display, cpu_transcoder));

	vsc_ext_stagger = REG_FIELD_GET(VSC_EXT_STAGGER_MASK, val);
	pps_stagger = REG_FIELD_GET(PPS_STAGGER_MASK, val);
	gmp_stagger = REG_FIELD_GET(GMP_STAGGER_MASK, val);

	crtc_state->dip.vsc_ext_sdp_tl =
		intel_dip_sdp_stagger_to_tl(crtc_state, vsc_ext_stagger);
	crtc_state->dip.pps_sdp_tl =
		intel_dip_sdp_stagger_to_tl(crtc_state, pps_stagger);
	crtc_state->dip.gmp_sdp_tl =
		intel_dip_sdp_stagger_to_tl(crtc_state, gmp_stagger);
}

void intel_dip_sdp_transmission_line_get_config(struct intel_crtc_state *crtc_state)
{
	crtc_state->dip.emp_as_sdp_tl = intel_dip_read_emp_as_sdp_tl(crtc_state);

	intel_dip_cmn_sdp_transmission_line_get_config(crtc_state);
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

	if (!HAS_COMMON_SDP_TL(display))
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

	if (!HAS_COMMON_SDP_TL(display))
		return;

	intel_de_write(display, CMN_SDP_TL(display, cpu_transcoder), 0);
}
