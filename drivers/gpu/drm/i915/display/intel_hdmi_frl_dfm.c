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

/* Get required no. of tribytes (estimate1) during HCBlank */
static u32
get_frl_hcblank_tb_est1_target(u32 hcactive_target_tb,
			       u32 hactive, u32 hblank)
{
	return DIV_ROUND_UP(hcactive_target_tb * hblank, hactive);
}

/* Get required no. of tribytes during HCBlank */
static u32
get_frl_hcblank_tb_target(u32 hcactive_target_tb, u32 hactive,
			  u32 hblank, u32 hcblank_audio_min,
			  u32 cfrl_available)
{
	u32 hcblank_target_tb1 = get_frl_hcblank_tb_est1_target(hcactive_target_tb,
									 hactive, hblank);
	u32 hcblank_target_tb2 = max(hcblank_target_tb1, hcblank_audio_min);

	return 4 * (min(hcblank_target_tb2,
			(2 * cfrl_available - 3 * hcactive_target_tb) / 2) / 4);
}

static u32
get_dsc_tribyte_time(u32 num_tribyte,
		     u32 tribyte_rate_k)
{
	u32 time_multiplier;
	u64 tribyte_time;

	/*
	 * tribyte_rate_k is the num of kilo tribytes in 1 sec, on an average.
	 *
	 * time taken for:
	 * (tribyte_rate_k * 1000) tribytes -> 1 sec
	 * (tribyte_rate_k * 1000) tribytes -> 10^9 nsec
	 * 1000 tribytes                    -> (10^9 / tribyte_rate_k) nsec
	 * 1 tribyte                        -> 10^9 / tribyte_rate_k * 1000)
	 * 1 tribyte                        -> 10^6 / tribyte_rate_k
	 * tribyte_time			    -> num_tribyte * 10 ^ 6 / (tribyte_rate_k)
	 *
	 * So, avg time for tribyte_time in nsec =
	 *				num_tribyte * 10 ^ 6 / (tribyte_rate_k)
	 */
	time_multiplier = DIV_ROUND_UP(FRL_TIMING_NS_MULTIPLIER, 1000);
	tribyte_time = mul_u32_u32(num_tribyte, time_multiplier);

	return DIV_ROUND_UP_ULL(tribyte_time, tribyte_rate_k);
}

/* Get time to send all tribytes in hcactive region in nsec*/
static u32
get_dsc_tactive_target_ns(u32 frl_lanes, u32 hcactive_target_tb,
			  u32 ftb_avg_k, u32 min_frl_char_rate_k,
			  u32 overhead_max)
{
	u32 avg_tribyte_time_ns, tribyte_time_ns;
	u32 num_chars_hcactive;
	u32 frl_char_rate_k;

	/* Avg time to transmit all active region tribytes */
	avg_tribyte_time_ns = get_dsc_tribyte_time(hcactive_target_tb, ftb_avg_k);

	/*
	 * 2 bytes in active region = 1 FRL characters
	 * 1 Tribyte in active region = 3/2 FRL characters
	 */
	num_chars_hcactive = DIV_ROUND_UP(hcactive_target_tb * 3, 2);

	/*
	 * FRL rate = lanes * frl character rate
	 * But actual bandwidth wil be less, due to FRL limitations so account
	 * for the overhead involved.
	 * FRL rate with overhead = FRL rate * (100 - overhead %) / 100
	 */
	frl_char_rate_k = frl_lanes * min_frl_char_rate_k;
	frl_char_rate_k = DIV_ROUND_UP_ULL(mul_u32_u32(frl_char_rate_k,
						       EFFICIENCY_MULTIPLIER - overhead_max),
					   EFFICIENCY_MULTIPLIER);

	/* Time to transmit all characters with FRL limitations */
	tribyte_time_ns	= get_dsc_tribyte_time(num_chars_hcactive, frl_char_rate_k);
	return max(avg_tribyte_time_ns, tribyte_time_ns);
}

/* Get TBdelta : borrowing in tribytes relative to avg tribyte rate */
static u32
get_dsc_tri_bytes_delta(u32 tactive_target_ns, u32 tblank_target_ns,
			u32 tactive_ref_ns, u32 tblank_ref_ns,
			u32 hcactive_target_tb, u32 ftb_avg_k,
			u32 hactive, u32 hblank,
			u32 line_time_ns)
{
	u32 tb_delta_limit;
	u32 hcblank_target_tb1 = get_frl_hcblank_tb_est1_target(hcactive_target_tb,
								    hactive, hblank);
	u32 tribytes = (hcactive_target_tb + hcblank_target_tb1);
	u32 tactive_avg_ns;

	if (tblank_ref_ns < tblank_target_ns) {
		tactive_avg_ns =
			div64_u64(mul_u32_u32(FRL_TIMING_NS_MULTIPLIER, hcactive_target_tb),
				  mul_u32_u32(ftb_avg_k, 1000));
		tb_delta_limit =
			DIV_ROUND_UP_ULL(mul_u32_u32(tactive_ref_ns - tactive_avg_ns, tribytes),
					 line_time_ns);
	} else {
		u32 t_delta_ns;

		if (tactive_target_ns > tactive_ref_ns)
			t_delta_ns = tactive_target_ns - tactive_ref_ns;
		else
			t_delta_ns = tactive_ref_ns - tactive_target_ns;
		tb_delta_limit = div_u64(mul_u32_u32(t_delta_ns, tribytes), line_time_ns);
	}

	return tb_delta_limit;
}

static u32
get_dsc_tri_bytes_borrowed(u32 ftb_avg_k,
			   u32 tactive_target_ns,
			   u32 hcactive_target)
{
	u32 hactive, tb_borrowed;

	hactive = DIV_ROUND_UP_ULL(mul_u32_u32(tactive_target_ns,
					       DIV_ROUND_UP(ftb_avg_k, 1000)),
				   1000);
	tb_borrowed = hactive - hcactive_target;

	return tb_borrowed;
}

/* Compute hcactive and hcblank tribytes for given dsc bpp setting */
static void
compute_dsc_tribytes(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 hcactive_target_tb;
	u32 hcblank_target_tb;
	u32 cfrl_available;
	u32 num_slices;
	u32 bytes_target;

	/* Assert for slice width ?*/
	if (!frl_dfm->config.slice_width)
		return;

	num_slices = DIV_ROUND_UP(frl_dfm->config.hactive, frl_dfm->config.slice_width);

	/* Get required no. of tribytes during HCActive */
	bytes_target = num_slices *
		       DIV_ROUND_UP(frl_dfm->config.target_bpp_16 * frl_dfm->config.slice_width,
				    8 * BPP_MULTIPLIER);

	hcactive_target_tb = DIV_ROUND_UP(bytes_target, 3);

	/* Get FRL Available characters */
	cfrl_available = ((EFFICIENCY_MULTIPLIER - frl_dfm->params.overhead_max) *
			  frl_dfm->params.cfrl_line) / EFFICIENCY_MULTIPLIER;

	hcblank_target_tb =
		get_frl_hcblank_tb_target(hcactive_target_tb,
					  frl_dfm->config.hactive,
					  frl_dfm->config.hblank,
					  frl_dfm->params.hblank_audio_min,
					  cfrl_available);

	frl_dfm->params.hcactive_target = hcactive_target_tb;
	frl_dfm->params.hcblank_target = hcblank_target_tb;
}

/* Check if audio supported with given dsc bpp and frl bandwidth */
static bool
audio_supported_with_dsc(struct intel_hdmi_frl_dfm *frl_dfm)
{
	return frl_dfm->params.hcblank_target >= frl_dfm->params.hblank_audio_min;
}

/* Is DFM timing requirement is met with DSC */
static
bool timing_req_met_with_dsc(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 ftb_avg_k;
	u32 tactive_ref_ns, tblank_ref_ns, tactive_target_ns, tblank_target_ns;
	u32 tb_borrowed, tb_delta, tb_worst;

	/* Get the avg no of tribytes sent per sec (Kbps) */
	ftb_avg_k = (frl_dfm->params.hcactive_target + frl_dfm->params.hcblank_target) *
		    DIV_ROUND_UP(frl_dfm->params.pixel_clock_max_khz,
				 frl_dfm->config.hactive + frl_dfm->config.hblank);

	/* Time to send Active tribytes in nanoseconds */
	tactive_ref_ns = DIV_ROUND_UP(frl_dfm->params.line_time_ns * frl_dfm->config.hactive,
				      frl_dfm->config.hactive + frl_dfm->config.hblank);

	/* Time to send Blanking tribytes in nanoseconds */
	tblank_ref_ns = DIV_ROUND_UP(frl_dfm->params.line_time_ns * frl_dfm->config.hblank,
				     frl_dfm->config.hactive + frl_dfm->config.hblank);

	tactive_target_ns = get_dsc_tactive_target_ns(frl_dfm->config.lanes,
						      frl_dfm->params.hcactive_target,
						      ftb_avg_k,
						      frl_dfm->params.char_rate_min_kbps,
						      frl_dfm->params.overhead_max);

	tblank_target_ns = frl_dfm->params.line_time_ns - tactive_target_ns;

	/* Get no. of tri bytes borrowed with DSC enabled */
	tb_borrowed = get_dsc_tri_bytes_borrowed(ftb_avg_k,
						 tactive_target_ns,
						 frl_dfm->params.hcactive_target);

	tb_delta = get_dsc_tri_bytes_delta(tactive_target_ns,
					   tblank_target_ns,
					   tactive_ref_ns,
					   tblank_ref_ns,
					   frl_dfm->params.hcactive_target,
					   ftb_avg_k,
					   frl_dfm->config.hactive,
					   frl_dfm->config.hblank,
					   frl_dfm->params.line_time_ns);

	tb_worst = max(tb_borrowed, tb_delta);
	if (tb_worst > TB_BORROWED_MAX)
		return false;

	frl_dfm->params.ftb_avg_k = ftb_avg_k;
	frl_dfm->params.tb_borrowed = tb_borrowed;

	return true;
}

/* Check Utilization constraint with DSC */
static bool
utilization_constraints_met_with_dsc(struct intel_hdmi_frl_dfm *frl_dfm)
{
	u32 hcactive_target_tb = frl_dfm->params.hcactive_target;
	u32 hcblank_target_tb = frl_dfm->params.hcblank_target;
	u32 frl_char_per_line = frl_dfm->params.cfrl_line;
	u32 overhead_max = frl_dfm->params.overhead_max;
	u32 utilization_with_overhead;
	u32 actual_frl_char_payload;
	u32 utilization;

	/*
	 * Note:
	 * 1 FRL characters per 2 bytes in active period
	 * 1 FRL char per byte in Blanking period
	 */
	actual_frl_char_payload = DIV_ROUND_UP(3 * hcactive_target_tb, 2) +
				  hcblank_target_tb;

	utilization = (actual_frl_char_payload * EFFICIENCY_MULTIPLIER) /
		      frl_char_per_line;

	/*
	 * Utilization with overhead = utlization% +overhead %
	 * should be less than 100%
	 */
	utilization_with_overhead = utilization + overhead_max;
	if (utilization_with_overhead  > EFFICIENCY_MULTIPLIER)
		return false;

	return true;
}

/*
 * intel_hdmi_frl_dfm_dsc_requirement_met : Check if FRL DFM requirements are met with
 * the given bpp.
 * @frl_dfm: dfm structure
 *
 * Returns true if the frl dfm requirements are met, else returns false.
 */
bool intel_hdmi_frl_dfm_dsc_requirement_met(struct intel_hdmi_frl_dfm *frl_dfm)
{
	if (!frl_dfm->config.slice_width || !frl_dfm->config.target_bpp_16)
		return false;

	compute_max_frl_link_overhead(frl_dfm);
	compute_link_characteristics(frl_dfm);
	compute_audio_hblank_min(frl_dfm);
	compute_dsc_tribytes(frl_dfm);

	if (!audio_supported_with_dsc(frl_dfm))
		return false;

	if (!timing_req_met_with_dsc(frl_dfm))
		return false;

	if (!utilization_constraints_met_with_dsc(frl_dfm))
		return false;

	return true;
}
