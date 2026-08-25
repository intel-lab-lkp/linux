// SPDX-License-Identifier: MIT
/*
 * Copyright 2024, Intel Corporation.
 */

#include <linux/debugfs.h>

#include <drm/drm_print.h>

#include "intel_alpm.h"
#include "intel_crtc.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "intel_display_utils.h"
#include "intel_dp.h"
#include "intel_dp_aux.h"
#include "intel_psr.h"
#include "intel_psr_regs.h"
#include "intel_vrr.h"

#define SILENCE_PERIOD_MIN_TIME	80
#define SILENCE_PERIOD_MAX_TIME	180
#define SILENCE_PERIOD_TIME	(SILENCE_PERIOD_MIN_TIME +	\
				(SILENCE_PERIOD_MAX_TIME -	\
				 SILENCE_PERIOD_MIN_TIME) / 2)

#define LFPS_CYCLE_COUNT 10

bool intel_alpm_aux_wake_supported(struct intel_dp *intel_dp)
{
	return intel_dp->alpm_dpcd & DP_ALPM_CAP;
}

bool intel_alpm_aux_less_wake_supported(struct intel_dp *intel_dp)
{
	return intel_dp->alpm_dpcd & DP_ALPM_AUX_LESS_CAP;
}

bool intel_alpm_is_alpm_aux_less(struct intel_dp *intel_dp,
				 const struct intel_crtc_state *crtc_state)
{
	return intel_psr_needs_alpm_aux_less(intel_dp, crtc_state) ||
		(crtc_state->has_lobf && intel_alpm_aux_less_wake_supported(intel_dp));
}

bool intel_alpm_source_supported(struct intel_connector *connector)
{
	struct intel_display *display = to_intel_display(connector);

	if (!((connector->base.connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
	       DISPLAY_VER(display) >= 35) ||
	    (connector->base.connector_type == DRM_MODE_CONNECTOR_eDP &&
	     DISPLAY_VER(display) >= 20)))
		return false;

	return true;
}

void intel_alpm_init_dpcd(struct intel_dp *intel_dp)
{
	u8 dpcd;

	if (drm_dp_dpcd_readb(&intel_dp->aux, DP_RECEIVER_ALPM_CAP, &dpcd) < 0)
		return;

	intel_dp->alpm_dpcd = dpcd;
}

void intel_alpm_init(struct intel_dp *intel_dp)
{
	mutex_init(&intel_dp->alpm.lock);
}

static int get_silence_period_symbols(const struct intel_crtc_state *crtc_state)
{
	return SILENCE_PERIOD_TIME * intel_dp_link_symbol_clock(crtc_state->port_clock) /
		1000 / 1000;
}

static void get_lfps_cycle_min_max_time(const struct intel_crtc_state *crtc_state,
					int *min, int *max)
{
	struct intel_display *display = to_intel_display(crtc_state);

	*min = 320;
	*max = 1600;
	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP)) {
		if (crtc_state->port_clock < 540000 && DISPLAY_VER(display) < 35) {
			*min = 65 * LFPS_CYCLE_COUNT;
			*max = 75 * LFPS_CYCLE_COUNT;
		} else {
			*min = 140;
			*max = 800;
		}
	}
}

static int get_lfps_cycle_time(const struct intel_crtc_state *crtc_state)
{
	int tlfps_cycle_min, tlfps_cycle_max;

	get_lfps_cycle_min_max_time(crtc_state, &tlfps_cycle_min,
				    &tlfps_cycle_max);

	return tlfps_cycle_min +  (tlfps_cycle_max - tlfps_cycle_min) / 2;
}

static int get_lfps_half_cycle_clocks(const struct intel_crtc_state *crtc_state)
{
	return get_lfps_cycle_time(crtc_state) * crtc_state->port_clock / 1000 /
		1000 / (2 * LFPS_CYCLE_COUNT);
}

#define ML_PHY_LOCK_LEN		252
#define ML_PHY_LOCK_LEN_UHBR	396

static int get_tphy2_p2_to_p0(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);

	return DISPLAY_VER(display) >= 35 ? (20 * 1000) : (12 * 1000);
}

#define DEFAULT_MAX_LTTPR_COUNT 7
#define MAX_SUPPORTED_DP_LINK_RATES 12

static const u32 establishment_periods[DEFAULT_MAX_LTTPR_COUNT][MAX_SUPPORTED_DP_LINK_RATES] = {
	/* 1620, 2160, 2430, 2700, 3240, 4320, 5400, 6750, 8100, 10000, 13500, 20000 */
	/* No Retimers */
	{ 61070, 0, 0, 56710, 0, 0, 53450, 0, 52360, 59050, 56750, 54620 },
	/*  1 Retimer  */
	{ 155670, 0, 0, 151310, 0, 0, 148050, 0, 146960, 153650, 151350, 149220 },
	/*  2 Retimers */
	{ 181160, 0, 0, 172450, 0, 0, 165910, 0, 163740, 177120, 172520, 168220 },
	/*  3 Retimers */
	{ 206650, 0, 0, 193580, 0, 0, 183780, 0, 180510, 200590, 193690, 187290 },
	/*  4 Retimers */
	{ 232140, 0, 0, 214710, 0, 0, 201650, 0, 197290, 224060, 214860, 206320 },
	/*  5 Retimers */
	{ 257620, 0, 0, 235850, 0, 0, 219510, 0, 214070, 247530, 236030, 225360 },
	/*  6 Retimers */
	{ 283110, 0, 0, 256980, 0, 0, 237380, 0, 230850, 271000, 257200, 244390 },
};

enum link_rate_idx {
	LINK_RATE_INVALID = -1,
	LINK_RATE_1,
	LINK_RATE_2,
	LINK_RATE_3,
	LINK_RATE_4,
	LINK_RATE_5,
	LINK_RATE_6,
	LINK_RATE_7,
	LINK_RATE_8,
	LINK_RATE_9,
	LINK_RATE_10,
	LINK_RATE_11,
	LINK_RATE_12
};

static int get_link_rate_index(int port_clock)
{
	int link_rate_index;

	switch (port_clock) {
	case 162000:
		link_rate_index = LINK_RATE_1;	// Rate_1 (RBR) - 1.62 Gbps/Lane
		break;
	case 216000:
		link_rate_index = LINK_RATE_2;	// Rate_2       - 2.16 Gbps/Lane
		break;
	case 243000:
		link_rate_index = LINK_RATE_3;	// Rate_3       - 2.43 Gbps/Lane
		break;
	case 270000:
		link_rate_index = LINK_RATE_4;	// Rate_4 (HBR) - 2.70 Gbps/Lane
		break;
	case 324000:
		link_rate_index = LINK_RATE_5;	// Rate_5 (RBR2)- 3.24 Gbps/Lane
		break;
	case 432000:
		link_rate_index = LINK_RATE_6;	// Rate_6       - 4.32 Gbps/Lane
		break;
	case 540000:
		link_rate_index = LINK_RATE_7;	// Rate_7 (HBR2)- 5.40 Gbps/Lane
		break;
	case 675000:
		link_rate_index = LINK_RATE_8;	// Rate_8       - 6.75 Gbps/Lane
		break;
	case 810000:
		link_rate_index = LINK_RATE_9;	// Rate_9 (HBR3)- 8.10 Gbps/Lane
		break;
	case 1000000:
		link_rate_index = LINK_RATE_10;	// Rate_10 (UHBR10) - 10.0 Gbps/Lane
		break;
	case 1350000:
		link_rate_index = LINK_RATE_11;	// Rate_11 (UHBR13.5) - 13.5 Gbps/Lane
		break;
	case 2000000:
		link_rate_index = LINK_RATE_12;	// Rate_12 (UHBR20) - 20.0 Gbps/Lane
		break;
	default:
		link_rate_index = -1;
		break;
	}

	return link_rate_index;
}

static int get_establishment_period(struct intel_dp *intel_dp,
				    const struct intel_crtc_state *crtc_state)
{
	int t1 = 50 * 1000;
	int tps4 = intel_dp_is_uhbr(crtc_state) ? (ML_PHY_LOCK_LEN_UHBR * 32) :
		   (ML_PHY_LOCK_LEN * 10);
	/* port_clock is link rate in 10kbit/s units */
	int tml_phy_lock = 1000 * 1000 * tps4 / crtc_state->port_clock / 10;
	int lttpr_count = 0;
	int tcds, establishment_period;

	if (intel_crtc_has_type(crtc_state, INTEL_OUTPUT_EDP)) {
		tcds = (7 + DIV_ROUND_UP(6500, tml_phy_lock) + 1) * tml_phy_lock;
	} else {
		int idx = get_link_rate_index(crtc_state->port_clock);

		tcds = 7 * tml_phy_lock;
		lttpr_count = drm_dp_lttpr_count(intel_dp->lttpr_common_caps);

		if (idx != LINK_RATE_INVALID &&
		    lttpr_count < DEFAULT_MAX_LTTPR_COUNT &&
		    establishment_periods[lttpr_count][idx]) {
			establishment_period = establishment_periods[lttpr_count][idx];
			return establishment_period;
		}
	}

	if (lttpr_count) {
		int tlw = 13000;
		int tcs = 10000;
		int tlfps_period = get_lfps_cycle_time(crtc_state);
		int tdcs = (SILENCE_PERIOD_TIME + t1 + tcs +
			    (lttpr_count - 1) * (tlw + tlfps_period));
		int tacds = 70000;
		int tds = (lttpr_count - 1) * 7 * tml_phy_lock;

		/* tdrl is same as tcds*/
		establishment_period = tlw + tlfps_period + tdcs + tacds + tds + tcds;
	} else {
		/* TODO: Add a check for data realign by DPCD 0x116[3] */

		establishment_period = (SILENCE_PERIOD_TIME + t1 + tcds);
	}

	return establishment_period;
}

/*
 * AUX-Less Wake Time = CEILING( ((PHY P2 to P0) + tLFPS_Period, Max+
 * tSilence, Max+ tPHY Establishment + tCDS) / tline)
 * For the "PHY P2 to P0" latency see the PHY Power Control page
 * (PHY P2 to P0) : https://gfxspecs.intel.com/Predator/Home/Index/68965
 * : 12 us
 * The tLFPS_Period, Max term is 800ns
 * The tSilence, Max term is 180ns
 * The tPHY Establishment (a.k.a. t1) term is 50us
 * The tCDS term is 1 or 2 times t2
 * t2 = Number ML_PHY_LOCK * tML_PHY_LOCK
 * Number ML_PHY_LOCK = ( 7 + CEILING( 6.5us / tML_PHY_LOCK ) + 1)
 * Rounding up the 6.5us padding to the next ML_PHY_LOCK boundary and
 * adding the "+ 1" term ensures all ML_PHY_LOCK sequences that start
 * within the CDS period complete within the CDS period regardless of
 * entry into the period
 * tML_PHY_LOCK = TPS4 Length * ( 10 / (Link Rate in MHz) )
 * ML_PHY_LOCK Length (8b/10b): 252
 * ML_PHY_LOCK Length (128b/132b): 396
 * TPS4 Length = ML_PHY_LOCK * Rate Div
 * Rate Div = 32 for 128b/132b and 10 for 8b/10b.
 */
static int _lnl_compute_aux_less_wake_time(struct intel_dp *intel_dp,
					   const struct intel_crtc_state *crtc_state)
{
	int tphy2_p2_to_p0 = get_tphy2_p2_to_p0(crtc_state);
	int establishment_period = get_establishment_period(intel_dp, crtc_state);

	return DIV_ROUND_UP(tphy2_p2_to_p0 + get_lfps_cycle_time(crtc_state) +
			    establishment_period, 1000);
}

static int
_lnl_compute_aux_less_alpm_params(struct intel_dp *intel_dp,
				  struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int aux_less_wake_time, aux_less_wake_lines, silence_period,
		lfps_half_cycle;

	aux_less_wake_time =
		_lnl_compute_aux_less_wake_time(intel_dp, crtc_state);
	aux_less_wake_lines = intel_usecs_to_scanlines(&crtc_state->hw.adjusted_mode,
						       aux_less_wake_time);
	silence_period = get_silence_period_symbols(crtc_state);

	lfps_half_cycle = get_lfps_half_cycle_clocks(crtc_state);

	if (aux_less_wake_lines > ALPM_CTL_AUX_LESS_WAKE_TIME_MASK ||
	    silence_period > PORT_ALPM_CTL_SILENCE_PERIOD_MASK ||
	    lfps_half_cycle > PORT_ALPM_LFPS_CTL_LAST_LFPS_HALF_CYCLE_DURATION_MASK)
		return false;

	if (display->params.psr_safest_params)
		aux_less_wake_lines = ALPM_CTL_AUX_LESS_WAKE_TIME_MASK;

	crtc_state->alpm_state.aux_less_wake_lines = aux_less_wake_lines;
	crtc_state->alpm_state.silence_period_sym_clocks = silence_period;
	crtc_state->alpm_state.lfps_half_cycle_num_of_syms = lfps_half_cycle;

	return true;
}

static bool _lnl_compute_alpm_params(struct intel_dp *intel_dp,
				     struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int check_entry_lines;

	if (DISPLAY_VER(display) < 20)
		return true;

	/* ALPM Entry Check = 2 + CEILING( 5us /tline ) */
	check_entry_lines = 2 +
		intel_usecs_to_scanlines(&crtc_state->hw.adjusted_mode, 5);

	if (check_entry_lines > 15)
		return false;

	if (!_lnl_compute_aux_less_alpm_params(intel_dp, crtc_state))
		return false;

	if (display->params.psr_safest_params)
		check_entry_lines = 15;

	crtc_state->alpm_state.check_entry_lines = check_entry_lines;

	return true;
}

/*
 * IO wake time for DISPLAY_VER < 12 is not directly mentioned in Bspec. There
 * are 50 us io wake time and 32 us fast wake time. Clearly preharge pulses are
 * not (improperly) included in 32 us fast wake time. 50 us - 32 us = 18 us.
 */
static int skl_io_buffer_wake_time(void)
{
	return 18;
}

static int tgl_io_buffer_wake_time(void)
{
	return 10;
}

static int io_buffer_wake_time(const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(crtc_state);

	if (DISPLAY_VER(display) >= 12)
		return tgl_io_buffer_wake_time();
	else
		return skl_io_buffer_wake_time();
}

bool intel_alpm_compute_params(struct intel_dp *intel_dp,
			       struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	int io_wake_lines, io_wake_time, fast_wake_lines, fast_wake_time;
	int tfw_exit_latency = 20; /* eDP spec */
	int phy_wake = 4;	   /* eDP spec */
	int preamble = 8;	   /* eDP spec */
	int precharge = intel_dp_aux_fw_sync_len(intel_dp) - preamble;
	u8 max_wake_lines;

	io_wake_time = max(precharge, io_buffer_wake_time(crtc_state)) +
		preamble + phy_wake + tfw_exit_latency;
	fast_wake_time = precharge + preamble + phy_wake +
		tfw_exit_latency;

	if (DISPLAY_VER(display) >= 20)
		max_wake_lines = 68;
	else if (DISPLAY_VER(display) >= 12)
		max_wake_lines = 12;
	else
		max_wake_lines = 8;

	io_wake_lines = intel_usecs_to_scanlines(
		&crtc_state->hw.adjusted_mode, io_wake_time);
	fast_wake_lines = intel_usecs_to_scanlines(
		&crtc_state->hw.adjusted_mode, fast_wake_time);

	if (io_wake_lines > max_wake_lines ||
	    fast_wake_lines > max_wake_lines)
		return false;

	if (!_lnl_compute_alpm_params(intel_dp, crtc_state))
		return false;

	if (display->params.psr_safest_params)
		io_wake_lines = fast_wake_lines = max_wake_lines;

	/* According to Bspec lower limit should be set as 7 lines. */
	crtc_state->alpm_state.io_wake_lines = max(io_wake_lines, 7);
	crtc_state->alpm_state.fast_wake_lines = max(fast_wake_lines, 7);

	return true;
}

int intel_alpm_lobf_min_guardband(struct intel_crtc_state *crtc_state)
{
	struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	int first_sdp_position = adjusted_mode->crtc_vtotal -
				 adjusted_mode->crtc_vsync_start;
	int waketime_in_lines;

	/*
	 * #FIXME: Need to check if io_wake_lines or aux_less_wake_lines
	 * is applicable. Currently this information is not readily
	 * available in crtc_state, so max will suffice for now.
	 */
	waketime_in_lines = max(crtc_state->alpm_state.io_wake_lines,
				crtc_state->alpm_state.aux_less_wake_lines);

	if (!crtc_state->has_lobf)
		return 0;

	return first_sdp_position + waketime_in_lines + crtc_state->set_context_latency;
}

static bool intel_alpm_lobf_is_window1_sufficient(struct intel_crtc_state *crtc_state)
{
	struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	int vblank = adjusted_mode->crtc_vtotal - adjusted_mode->crtc_vdisplay;
	int window1;

	/*
	 * LOBF must be disabled if the number of lines within Window 1 is not
	 * greater than ALPM_CTL[ALPM Entry Check]
	 */
	window1 = vblank - min(vblank,
			       crtc_state->vrr.guardband +
			       crtc_state->set_context_latency);

	return window1 > crtc_state->alpm_state.check_entry_lines;
}

void intel_alpm_lobf_compute_config_late(struct intel_dp *intel_dp,
					 struct intel_crtc_state *crtc_state)
{
	struct drm_display_mode *adjusted_mode = &crtc_state->hw.adjusted_mode;
	int waketime_in_lines, first_sdp_position;

	if (!crtc_state->has_lobf)
		return;

	if (crtc_state->has_psr ||
	    !intel_vrr_is_fixed_rr(crtc_state) ||
	    !intel_alpm_lobf_is_window1_sufficient(crtc_state)) {
		crtc_state->has_lobf = false;
		return;
	}

	/*
	 * LOBF can only be enabled if the time from the start of the SCL+Guardband
	 * window to the position of the first SDP is greater than the time it takes
	 * to wake the main link.
	 *
	 * Position of first sdp : vsync_start
	 * start of scl + guardband : vtotal - (scl + guardband)
	 * time in lines to wake main link : waketime_in_lines
	 *
	 * Position of first sdp - start of (scl + guardband) > time in lines to wake main link
	 * vsync_start - (vtotal - (scl + guardband)) > waketime_in_lines
	 * vsync_start - vtotal + scl + guardband > waketime_in_lines
	 * scl + guardband > waketime_in_lines + (vtotal - vsync_start)
	 */
	first_sdp_position = adjusted_mode->crtc_vtotal - adjusted_mode->crtc_vsync_start;
	if (intel_alpm_aux_less_wake_supported(intel_dp))
		waketime_in_lines = crtc_state->alpm_state.io_wake_lines;
	else
		waketime_in_lines = crtc_state->alpm_state.aux_less_wake_lines;

	crtc_state->has_lobf = (crtc_state->set_context_latency + crtc_state->vrr.guardband) >
			       (first_sdp_position + waketime_in_lines);
}

void intel_alpm_lobf_compute_config(struct intel_dp *intel_dp,
				    struct intel_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state)
{
	struct intel_display *display = to_intel_display(intel_dp);

	if (intel_dp->alpm.lobf_disable_debug) {
		drm_dbg_kms(display->drm, "LOBF is disabled by debug flag\n");
		return;
	}

	if (intel_dp->alpm.sink_alpm_error)
		return;

	if (!intel_dp_is_edp(intel_dp))
		return;

	if (DISPLAY_VER(display) < 20)
		return;

	if (!intel_dp->as_sdp_supported)
		return;

	if (!intel_vrr_always_use_vrr_tg(display))
		return;

	if (!(intel_alpm_aux_wake_supported(intel_dp) ||
	      intel_alpm_aux_less_wake_supported(intel_dp)))
		return;

	if (!intel_alpm_compute_params(intel_dp, crtc_state))
		return;

	crtc_state->has_lobf = true;
}

static u32 get_pr_alpm_as_sdp_transmission_time(const struct intel_crtc_state *crtc_state)
{
	u8 as_sdp_setup_time = intel_dp_as_sdp_transmission_time();

	switch (as_sdp_setup_time) {
	case DP_PR_AS_SDP_SETUP_TIME_T1:
		return PR_ALPM_CTL_ADAPTIVE_SYNC_SDP_POSITION_T1;
	case DP_PR_AS_SDP_SETUP_TIME_DYNAMIC:
		return PR_ALPM_CTL_ADAPTIVE_SYNC_SDP_POSITION_T1_OR_T2;
	case DP_PR_AS_SDP_SETUP_TIME_T2:
		return PR_ALPM_CTL_ADAPTIVE_SYNC_SDP_POSITION_T2;
	default:
		MISSING_CASE(as_sdp_setup_time);
		return PR_ALPM_CTL_ADAPTIVE_SYNC_SDP_POSITION_T1;
	}
}

static void lnl_alpm_configure(struct intel_dp *intel_dp,
			       const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	enum transcoder cpu_transcoder = crtc_state->cpu_transcoder;
	u32 alpm_ctl;

	if (DISPLAY_VER(display) < 20 || (!intel_psr_needs_alpm(intel_dp, crtc_state) &&
					  !crtc_state->has_lobf))
		return;

	mutex_lock(&intel_dp->alpm.lock);
	/*
	 * Panel Replay on eDP is always using ALPM aux less. I.e. no need to
	 * check panel support at this point.
	 */
	if (intel_alpm_is_alpm_aux_less(intel_dp, crtc_state)) {
		alpm_ctl = ALPM_CTL_ALPM_ENABLE |
			ALPM_CTL_ALPM_AUX_LESS_ENABLE |
			ALPM_CTL_AUX_LESS_SLEEP_HOLD_TIME_50_SYMBOLS;

		if (DISPLAY_VER(display) < 35)
			alpm_ctl |= ALPM_CTL_AUX_LESS_WAKE_TIME(crtc_state->alpm_state.aux_less_wake_lines);
		else
			alpm_ctl |= ALPM_CTL_AUX_LESS_WAKE_TIME_XE3LPD(crtc_state->alpm_state.aux_less_wake_lines);

		if (intel_dp->as_sdp_supported) {
			u32 pr_alpm_ctl = get_pr_alpm_as_sdp_transmission_time(crtc_state);

			if (crtc_state->link_off_after_as_sdp_when_pr_active)
				pr_alpm_ctl |= PR_ALPM_CTL_ALLOW_LINK_OFF_BETWEEN_AS_SDP_AND_SU;
			if (crtc_state->disable_as_sdp_when_pr_active)
				pr_alpm_ctl |= PR_ALPM_CTL_AS_SDP_TRANSMISSION_IN_ACTIVE_DISABLE;

			if (intel_display_power_dc3co_allowed(display))
				pr_alpm_ctl |= PR_ALPM_CTL_USE_DC3CO_IDLE_PROTOCOL;
			else
				pr_alpm_ctl &= ~PR_ALPM_CTL_USE_DC3CO_IDLE_PROTOCOL;

			intel_de_write(display, PR_ALPM_CTL(display, cpu_transcoder),
				       pr_alpm_ctl);
		}

	} else {
		alpm_ctl = ALPM_CTL_EXTENDED_FAST_WAKE_ENABLE |
			ALPM_CTL_EXTENDED_FAST_WAKE_TIME(crtc_state->alpm_state.fast_wake_lines);
	}

	if (crtc_state->has_lobf) {
		alpm_ctl |= ALPM_CTL_LOBF_ENABLE;
		drm_dbg_kms(display->drm, "Link off between frames (LOBF) enabled\n");
	}

	alpm_ctl |= ALPM_CTL_ALPM_ENTRY_CHECK(crtc_state->alpm_state.check_entry_lines);

	intel_de_write(display, ALPM_CTL(display, cpu_transcoder), alpm_ctl);
	mutex_unlock(&intel_dp->alpm.lock);
}

void intel_alpm_configure(struct intel_dp *intel_dp,
			  const struct intel_crtc_state *crtc_state)
{
	lnl_alpm_configure(intel_dp, crtc_state);
	intel_dp->alpm.transcoder = crtc_state->cpu_transcoder;
}

void intel_alpm_port_configure(struct intel_dp *intel_dp,
			       const struct intel_crtc_state *crtc_state)
{
	struct intel_display *display = to_intel_display(intel_dp);
	enum port port = dp_to_dig_port(intel_dp)->base.port;
	u32 alpm_ctl_val = 0, lfps_ctl_val = 0;

	if (DISPLAY_VER(display) < 20)
		return;

	if (intel_alpm_is_alpm_aux_less(intel_dp, crtc_state)) {
		alpm_ctl_val = PORT_ALPM_CTL_ALPM_AUX_LESS_ENABLE |
			PORT_ALPM_CTL_MAX_PHY_SWING_SETUP(15) |
			PORT_ALPM_CTL_MAX_PHY_SWING_HOLD(0) |
			PORT_ALPM_CTL_SILENCE_PERIOD(
				crtc_state->alpm_state.silence_period_sym_clocks);
		lfps_ctl_val = PORT_ALPM_LFPS_CTL_LFPS_CYCLE_COUNT(LFPS_CYCLE_COUNT) |
			PORT_ALPM_LFPS_CTL_LFPS_HALF_CYCLE_DURATION(
				crtc_state->alpm_state.lfps_half_cycle_num_of_syms) |
			PORT_ALPM_LFPS_CTL_FIRST_LFPS_HALF_CYCLE_DURATION(
				crtc_state->alpm_state.lfps_half_cycle_num_of_syms) |
			PORT_ALPM_LFPS_CTL_LAST_LFPS_HALF_CYCLE_DURATION(
				crtc_state->alpm_state.lfps_half_cycle_num_of_syms);
	}

	intel_de_write(display, PORT_ALPM_CTL(port), alpm_ctl_val);

	intel_de_write(display, PORT_ALPM_LFPS_CTL(port), lfps_ctl_val);
}

void intel_alpm_lobf_disable(const struct intel_crtc_state *new_crtc_state)
{
	struct intel_display *display = to_intel_display(new_crtc_state);
	enum transcoder cpu_transcoder = new_crtc_state->cpu_transcoder;
	struct intel_encoder *encoder;

	for_each_intel_encoder_mask(display->drm, encoder,
				    new_crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp;

		if (!intel_encoder_is_dp(encoder))
			continue;

		intel_dp = enc_to_intel_dp(encoder);

		if (!intel_dp_is_edp(intel_dp))
			continue;

		mutex_lock(&intel_dp->alpm.lock);
		intel_de_write(display, ALPM_CTL(display, cpu_transcoder), 0);
		drm_dbg_kms(display->drm, "Link off between frames (LOBF) disabled\n");
		mutex_unlock(&intel_dp->alpm.lock);
	}
}

void intel_alpm_enable_sink(struct intel_dp *intel_dp,
			    const struct intel_crtc_state *crtc_state)
{
	u8 val;

	if (!intel_psr_needs_alpm(intel_dp, crtc_state) && !crtc_state->has_lobf)
		return;

	val = DP_ALPM_ENABLE | DP_ALPM_LOCK_ERROR_IRQ_HPD_ENABLE;

	if (crtc_state->has_panel_replay || (crtc_state->has_lobf &&
					     intel_alpm_aux_less_wake_supported(intel_dp)))
		val |= DP_ALPM_MODE_AUX_LESS;

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_RECEIVER_ALPM_CONFIG, val);
}

void intel_alpm_lobf_enable(const struct intel_crtc_state *new_crtc_state)
{
	struct intel_display *display = to_intel_display(new_crtc_state);
	struct intel_encoder *encoder;

	for_each_intel_encoder_mask(display->drm, encoder,
				    new_crtc_state->uapi.encoder_mask) {
		struct intel_dp *intel_dp;

		if (!intel_encoder_is_dp(encoder))
			continue;

		intel_dp = enc_to_intel_dp(encoder);

		if (intel_dp_is_edp(intel_dp)) {
			intel_alpm_enable_sink(intel_dp, new_crtc_state);
			intel_alpm_configure(intel_dp, new_crtc_state);
		}
	}
}

static int i915_edp_lobf_info_show(struct seq_file *m, void *data)
{
	struct intel_connector *connector = m->private;
	struct intel_display *display = to_intel_display(connector);
	struct drm_crtc *crtc;
	struct intel_crtc_state *crtc_state;
	enum transcoder cpu_transcoder;
	u32 alpm_ctl;
	int ret;

	ret = drm_modeset_lock_single_interruptible(&display->drm->mode_config.connection_mutex);
	if (ret)
		return ret;

	crtc = connector->base.state->crtc;
	if (connector->base.status != connector_status_connected || !crtc) {
		ret = -ENODEV;
		goto out;
	}

	crtc_state = to_intel_crtc_state(crtc->state);
	cpu_transcoder = crtc_state->cpu_transcoder;
	alpm_ctl = intel_de_read(display, ALPM_CTL(display, cpu_transcoder));
	seq_printf(m, "LOBF status: %s\n", str_enabled_disabled(alpm_ctl & ALPM_CTL_LOBF_ENABLE));
	seq_printf(m, "Aux-wake alpm status: %s\n",
		   str_enabled_disabled(!(alpm_ctl & ALPM_CTL_ALPM_AUX_LESS_ENABLE)));
	seq_printf(m, "Aux-less alpm status: %s\n",
		   str_enabled_disabled(alpm_ctl & ALPM_CTL_ALPM_AUX_LESS_ENABLE));
out:
	drm_modeset_unlock(&display->drm->mode_config.connection_mutex);

	return ret;
}

DEFINE_SHOW_ATTRIBUTE(i915_edp_lobf_info);

static int
i915_edp_lobf_debug_get(void *data, u64 *val)
{
	struct intel_connector *connector = data;
	struct intel_dp *intel_dp = enc_to_intel_dp(connector->encoder);

	*val = intel_dp->alpm.lobf_disable_debug;

	return 0;
}

static int
i915_edp_lobf_debug_set(void *data, u64 val)
{
	struct intel_connector *connector = data;
	struct intel_dp *intel_dp = enc_to_intel_dp(connector->encoder);

	intel_dp->alpm.lobf_disable_debug = val;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(i915_edp_lobf_debug_fops,
			i915_edp_lobf_debug_get, i915_edp_lobf_debug_set,
			"%llu\n");

void intel_alpm_lobf_debugfs_add(struct intel_connector *connector)
{
	struct dentry *root = connector->base.debugfs_entry;

	if (!intel_alpm_source_supported(connector))
		return;

	debugfs_create_file("i915_edp_lobf_debug", 0644, root,
			    connector, &i915_edp_lobf_debug_fops);

	debugfs_create_file("i915_edp_lobf_info", 0444, root,
			    connector, &i915_edp_lobf_info_fops);
}

void intel_alpm_disable(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	enum transcoder cpu_transcoder = intel_dp->alpm.transcoder;

	if (DISPLAY_VER(display) < 20 || !intel_dp->alpm_dpcd)
		return;

	mutex_lock(&intel_dp->alpm.lock);

	intel_de_rmw(display, ALPM_CTL(display, cpu_transcoder),
		     ALPM_CTL_ALPM_ENABLE | ALPM_CTL_LOBF_ENABLE, 0);

	drm_dbg_kms(display->drm, "Disabling ALPM\n");
	mutex_unlock(&intel_dp->alpm.lock);
}

bool intel_alpm_get_error(struct intel_dp *intel_dp)
{
	struct intel_display *display = to_intel_display(intel_dp);
	struct drm_dp_aux *aux = &intel_dp->aux;
	u8 val;
	int r;

	r = drm_dp_dpcd_readb(aux, DP_RECEIVER_ALPM_STATUS, &val);
	if (r != 1) {
		drm_err(display->drm, "Error reading ALPM status\n");
		return true;
	}

	if (val & DP_ALPM_LOCK_TIMEOUT_ERROR) {
		drm_dbg_kms(display->drm, "ALPM lock timeout error\n");

		/* Clearing error */
		drm_dp_dpcd_writeb(aux, DP_RECEIVER_ALPM_STATUS, val);
		return true;
	}

	return false;
}
