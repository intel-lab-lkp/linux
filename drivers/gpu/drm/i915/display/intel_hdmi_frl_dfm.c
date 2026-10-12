// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corp
 */

#include <linux/kernel.h>

#include <drm/drm_connector.h>

#include "intel_hdmi_frl_dfm.h"

/* DFM constraints and tolerance values */
#define TB_BORROWED_MAX			400
#define FRL_CHAR_PER_CHAR_BLK		510
/* Tolerance pixel clock unit is in  mHz */
#define TOLERANCE_PIXEL_CLOCK		5

/* ppm base for the tolerances below */
#define PARTS_PER_MILLION		1000000
#define TOLERANCE_FRL_BIT_RATE		300
#define TOLERANCE_AUDIO_CLOCK		1000
#define ACR_RATE_MAX			1500
#define EFFICIENCY_MULTIPLIER		1000
#define OVERHEAD_M			(3 * EFFICIENCY_MULTIPLIER / 1000)
#define BPP_MULTIPLIER			16
#define FRL_TIMING_NS_MULTIPLIER	1000000000

/* Total FRL characters per super block */
static u32 get_frl_char_per_super_blk(u32 lanes)
{
	return (4 * FRL_CHAR_PER_CHAR_BLK) + lanes;
}

/* Total minimum overhead multiplied by EFFICIENCY_MULIPLIER */
static u32 get_total_minimum_overhead(u32 lanes)
{
	u32 overhead_sb;
	u32 overhead_rs;
	u32 overhead_map;

	/*
	 * Determine the overhead due to the inclusion of
	 * the SR and SSB FRL characters used for
	 * super block framing
	 */
	overhead_sb = (lanes * EFFICIENCY_MULTIPLIER) / get_frl_char_per_super_blk(lanes);

	/*
	 * Determine the overhead due to the inclusion of RS FEC pairity
	 * symbols. Each character block uses 8 FRL characters for RS Pairity
	 * and there are 4 character blocks per super block
	 */
	overhead_rs = (8 * 4 * EFFICIENCY_MULTIPLIER) /  get_frl_char_per_super_blk(lanes);

	/*
	 * Determine the overhead due to FRL Map characters.
	 * In a bandwidth constrained application, the FRL packets will be long,
	 * there will typically be two FRL Map Characters per Super Block most of the time.
	 * When a tracnsition occurs between Hactive and Hblank (uncomperssed video) or
	 * HCactive and HCblank (compressed video transport), there may be a
	 * third FRL Map Charecter. Therefore this spec assumes 2.5 FRL Map Characters
	 * per Super Block.
	 */
	overhead_map =
		(25  * EFFICIENCY_MULTIPLIER) / (10 * get_frl_char_per_super_blk(lanes));

	return overhead_sb + overhead_rs + overhead_map;
}

/* Audio Support Verification Computations */

/*
 * During the Hblank period, Audio packets (32 frl characters each),
 * ACR packets (32 frl characters each), Island guard band (4 total frl characters)
 * and Video guard band (3 frl characters) do not benefit from RC compression
 * Therefore start by determining the number of Control Characters that maybe
 * RC compressible
 */
static u32
get_num_char_rc_compressible(u32 color_format, u32 bpc,
			     u32 audio_packets_line, u32 hblank)
{
	u32 cfrl_free;
	u32 kcdx100, k420;

	if (color_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR420)
		k420 = 2;
	else
		k420 = 1;

	if (color_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR422)
		kcdx100 = 100;
	else
		kcdx100 = (100 * bpc) / 8;

	cfrl_free = max(((hblank * kcdx100) / (100 * k420) - 32 * audio_packets_line - 7),
			U32_MIN);

	return cfrl_free;
}

/*
 * Determine the actual number of characters made available by
 * RC compression
 */
static u32
get_num_char_compression_savings(u32 cfrl_free)
{
	/*
	 * In order to be conservative, situations are considered where
	 * maximum RC compression may not be possible.
	 * Add one character each for RC break caused by:
	 * • Island Preamble not aligned to the RC Compression
	 * • Video Preamble not aligned to the RC Compression
	 * • HSYNC lead edge not aligned to the RC Compression
	 * • HSYNC trail edge not aligned to the RC Compression
	 */
	const u32 cfrl_margin = 4;
	u32 cfrl_savings = max(((7 * cfrl_free) / 8) - cfrl_margin, U32_MIN);

	return cfrl_savings;
}

static u32
get_frl_bits_per_pixel(u32 color_format, u32 bpc)
{
	u32 kcdx100, k420;

	if (color_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR420)
		k420 = 2;
	else
		k420 = 1;

	if (color_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR422)
		kcdx100 = 100;
	else
		kcdx100 = (100 * bpc) / 8;

	return (24 * kcdx100) / (100 * k420);
}

/* Determine the total available tribytes during the blanking period */
static u32
get_blanking_tribytes_avail(u32 color_format,
			    u32 hblank, u32 bpc)
{
	u32 kcdx100, k420;

	if (color_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR420)
		k420 = 2;
	else
		k420 = 1;

	if (color_format == DRM_OUTPUT_COLOR_FORMAT_YCBCR422)
		kcdx100 = 100;
	else
		kcdx100 = (100 * bpc) / 8;

	return DIV_ROUND_UP((hblank * kcdx100), (100 * k420));
}

/*
 * Determine the minimum time necessary to transmit the active tribytes
 * considering frl bandwidth limitation.
 * Given the available bandwidth (i.e after overhead is considered),
 * tactive_min represents the amount of time needed to transmit all the
 * active data
 */
static u32
get_tactive_min(u32 num_lanes, u32 tribyte_active,
		u32 overhead_max_k, u32 frl_char_min_rate_k)
{
	u32 active_bytes, rate_kbps, efficiency_k, effective_rate_kbps;

	active_bytes = (3 * tribyte_active) / 2;
	rate_kbps = num_lanes * frl_char_min_rate_k;
	efficiency_k = EFFICIENCY_MULTIPLIER - overhead_max_k;
	effective_rate_kbps = mult_frac(rate_kbps, efficiency_k, EFFICIENCY_MULTIPLIER);

	return div_u64(mul_u32_u32(FRL_TIMING_NS_MULTIPLIER, active_bytes),
		       effective_rate_kbps) / 1000;
}

/*
 * Determine the minimum time necessary to transmit the video blanking
 * tribytes considering frl bandwidth limitations
 */
static u32
get_tblank_min(u32 num_lanes, u32 tribyte_blank,
	       u32 overhead_max_k, u32 frl_char_min_rate_k)
{
	u32 blank_bytes, rate_kbps, efficiency_k, effective_rate_kbps;

	blank_bytes = (3 * tribyte_blank) / 2;
	rate_kbps = num_lanes * frl_char_min_rate_k;
	efficiency_k = EFFICIENCY_MULTIPLIER - overhead_max_k;
	effective_rate_kbps = mult_frac(rate_kbps, efficiency_k, EFFICIENCY_MULTIPLIER);

	return div_u64(mul_u32_u32(FRL_TIMING_NS_MULTIPLIER, blank_bytes),
		       effective_rate_kbps) / 1000;
}

/* Collect link characteristics */
static void
compute_link_characteristics(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 frl_bit_rate_min_kbps, line_width, rate_m;

	/* Determine the maximum legal pixel rate */
	frl_dfm->params.pixel_clock_max_khz =
		(frl_dfm->config.pixel_clock_nominal_khz * (1000 + TOLERANCE_PIXEL_CLOCK)) / 1000;

	/* Determine the minimum Video Line period */
	line_width = frl_dfm->config.hactive + frl_dfm->config.hblank;

	frl_dfm->params.line_time_ns =
		div_u64(mul_u32_u32(FRL_TIMING_NS_MULTIPLIER, line_width),
			frl_dfm->params.pixel_clock_max_khz) / 1000;

	/* Determine the worst-case slow FRL Bit Rate in kbps*/
	frl_bit_rate_min_kbps =
		(frl_dfm->config.bit_rate_kbps / PARTS_PER_MILLION) *
		(PARTS_PER_MILLION - TOLERANCE_FRL_BIT_RATE);

	/* Determine the worst-case slow FRL Character Rate */
	frl_dfm->params.char_rate_min_kbps = frl_bit_rate_min_kbps / 18;

	/* Character rate in mega chars/sec */
	rate_m = DIV_ROUND_UP(frl_dfm->params.char_rate_min_kbps * frl_dfm->config.lanes, 1000);

	/* Determine the Minimum Total FRL characters per line period */
	frl_dfm->params.cfrl_line = DIV_ROUND_UP(frl_dfm->params.line_time_ns * rate_m,
						 FRL_TIMING_NS_MULTIPLIER / 1000000);
}

/* Determine FRL link overhead */
static void compute_max_frl_link_overhead(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 overhead_min = get_total_minimum_overhead(frl_dfm->config.lanes);

	/*
	 * Additional margin to the overhead is provided to account for the possibility
	 * of more Map Characters, zero padding at the end of HCactive, and other minor
	 * items
	 */
	frl_dfm->params.overhead_max = overhead_min + OVERHEAD_M;
}

/* Audio support Verification computations */
static void
compute_audio_hblank_min(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u64 num_audio_pkt_x100, rate;
	u32 audio_pkt_rate;

	/*
	 * #TODO: get the actual audio pkt type for the AP factor
	 * For now consider only L-PCM:
	 * Layout 0 (<= 2ch) AP = 0.25
	 * Layout 1 (> 2 ch) AP = 1.0
	 * Scale x100.
	 */
	num_audio_pkt_x100 = (frl_dfm->config.audio_channels <= 2) ? 25 : 100;

	/*
	 * Determine Audio Related Packet Rate considering the audio clock
	 * increased to maximim rate permitted by Tolerance Audio clock
	 */
	rate = frl_dfm->config.audio_hz *  num_audio_pkt_x100 + (2 * ACR_RATE_MAX * 100);

	audio_pkt_rate = DIV64_U64_ROUND_UP(rate * (PARTS_PER_MILLION + TOLERANCE_AUDIO_CLOCK),
					    (u64)PARTS_PER_MILLION * 100);

	/*
	 * Average required packets per line is
	 * number of audio packets needed during Hblank
	 */
	frl_dfm->params.num_audio_pkts_line =
		DIV_ROUND_UP(audio_pkt_rate * frl_dfm->params.line_time_ns,
			     FRL_TIMING_NS_MULTIPLIER);

	/*
	 * Minimum required Hblank assuming no Control Period RC Compression
	 * This includes Video Guard band, Two Island Guard bands, two 12 character
	 * Control Periods and 32 * AudioPackets_Line.
	 * In addition, 32 character periods are allocated for the transmission of an
	 * ACR packet
	 */
	frl_dfm->params.hblank_audio_min = 32 + 32 * frl_dfm->params.num_audio_pkts_line;
}

/*
 * Determine the number of tribytes required for active video , blanking period
 * with the pixel configuration
 */
static void
compute_tbactive_tbblank(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 bpp, bytes_per_line;

	bpp = get_frl_bits_per_pixel(frl_dfm->config.color_format, frl_dfm->config.bpc);
	bytes_per_line = (bpp * frl_dfm->config.hactive) / 8;

	frl_dfm->params.tb_active = DIV_ROUND_UP(bytes_per_line, 3);

	frl_dfm->params.tb_blank =
		get_blanking_tribytes_avail(frl_dfm->config.color_format,
					    frl_dfm->config.hblank,
					    frl_dfm->config.bpc);
}

/* Verify the configuration meets the capacity requirements for the FRL configuration*/
static bool
verify_frl_capacity_requirement(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 tactive_ref_ns, tblank_ref_ns, tactive_min_ns, tblank_min_ns;
	u32 tborrowed_ns;
	u32 line_time_ns = frl_dfm->params.line_time_ns;
	u32 hactive = frl_dfm->config.hactive;
	u32 hblank = frl_dfm->config.hblank;

	/* Determine the average tribyte rate in kilo tribytes per sec */
	frl_dfm->params.ftb_avg_k =
		div_u64(mul_u32_u32(frl_dfm->params.pixel_clock_max_khz,
				    frl_dfm->params.tb_active + frl_dfm->params.tb_blank),
			frl_dfm->config.hactive + frl_dfm->config.hblank);
	/*
	 * Determine the time required to transmit the active portion of the
	 * minimum possible active line period in the base timing
	 */
	tactive_ref_ns = (line_time_ns * hactive) / (hblank + hactive);

	/*
	 * Determine the time required to transmit the Video blanking portion
	 * of the minimum possible active line period in the base timing
	 */
	tblank_ref_ns = (line_time_ns * hblank) / (hblank + hactive);

	tactive_min_ns = get_tactive_min(frl_dfm->config.lanes,
					 frl_dfm->params.tb_active,
					 frl_dfm->params.overhead_max,
					 frl_dfm->params.char_rate_min_kbps);
	tblank_min_ns = get_tblank_min(frl_dfm->config.lanes,
				       frl_dfm->params.tb_blank,
				       frl_dfm->params.overhead_max,
				       frl_dfm->params.char_rate_min_kbps);

	if (tactive_ref_ns >= tactive_min_ns &&
	    tblank_ref_ns >= tblank_min_ns) {
		tborrowed_ns = 0;
		frl_dfm->params.tb_borrowed = 0;

		return true;
	}

	if (tactive_ref_ns < tactive_min_ns &&
	    tblank_ref_ns >= tblank_min_ns) {
		tborrowed_ns = tactive_min_ns - tactive_ref_ns;
		/* Determine the disparity in tribytes */
		frl_dfm->params.tb_borrowed =
			DIV_ROUND_UP_ULL(mul_u32_u32(tborrowed_ns, frl_dfm->params.ftb_avg_k) * 1000,
					 FRL_TIMING_NS_MULTIPLIER);

		if (frl_dfm->params.tb_borrowed <= TB_BORROWED_MAX)
			return true;
	}

	return false;
}

/* Verify utilization does not exceed capacity */
static bool
verify_utilization_possible(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 cfrl_free, cfrl_savings, frl_char_payload_actual;
	u32 utilization, margin;

	cfrl_free = get_num_char_rc_compressible(frl_dfm->config.color_format,
						 frl_dfm->config.bpc,
						 frl_dfm->params.num_audio_pkts_line,
						 frl_dfm->config.hblank);
	cfrl_savings = get_num_char_compression_savings(cfrl_free);

	/*
	 * Determine the actual number of payload FRL characters required to
	 * carry each video line
	 */
	frl_char_payload_actual =
		DIV_ROUND_UP(3 * frl_dfm->params.tb_active, 2) +
		frl_dfm->params.tb_blank - cfrl_savings;

	/*
	 * Determine the payload utilization of the total number of
	 * FRL characters
	 */
	utilization = (frl_char_payload_actual * EFFICIENCY_MULTIPLIER) / frl_dfm->params.cfrl_line;

	margin = 1000 - (utilization + frl_dfm->params.overhead_max);

	if (margin > 0)
		return true;

	return false;
}

/* Check if DFM requirement is met */
bool
intel_hdmi_frl_dfm_nondsc_requirement_met(struct intel_hdmi_frl_dfm *frl_dfm)
{
	bool frl_capacity_req_met;

	compute_max_frl_link_overhead(frl_dfm);
	compute_link_characteristics(frl_dfm);
	compute_audio_hblank_min(frl_dfm);
	compute_tbactive_tbblank(frl_dfm);

	if (frl_dfm->params.hblank_audio_min > frl_dfm->params.tb_blank)
		return false;

	frl_capacity_req_met = verify_frl_capacity_requirement(frl_dfm);

	if (frl_capacity_req_met)
		return verify_utilization_possible(frl_dfm);

	return false;
}
