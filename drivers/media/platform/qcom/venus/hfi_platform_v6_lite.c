// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025, The Linux Foundation. All rights reserved.
 */
#include "hfi_platform.h"

static const struct hfi_plat_caps caps[] = {
{
	.codec = HFI_VIDEO_CODEC_H264,
	.domain = VIDC_SESSION_TYPE_DEC,
	.caps[0] = {HFI_CAPABILITY_FRAME_WIDTH, 128, 1920, 1},
	.caps[1] = {HFI_CAPABILITY_FRAME_HEIGHT, 128, 1080, 1},
	.caps[2] = {HFI_CAPABILITY_MBS_PER_FRAME, 64, 8160, 1},
	.caps[3] = {HFI_CAPABILITY_BITRATE, 1, 60000000, 1 },
	.caps[4] = {HFI_CAPABILITY_MBS_PER_SECOND, 64, 244800, 1},
	.caps[5] = {HFI_CAPABILITY_FRAMERATE, 1, 120, 1},
	.caps[6] = {HFI_CAPABILITY_MAX_VIDEOCORES, 0, 1, 1},
	.num_caps = 7,
	.pl[0] = {HFI_H264_PROFILE_BASELINE, HFI_H264_LEVEL_1},
	.pl[1] = {HFI_H264_PROFILE_MAIN, HFI_H264_LEVEL_41},
	.pl[2] = {HFI_H264_PROFILE_HIGH, HFI_H264_LEVEL_5},
	.pl[3] = {HFI_H264_PROFILE_CONSTRAINED_BASE, HFI_H264_LEVEL_41},
	.pl[4] = {HFI_H264_PROFILE_CONSTRAINED_HIGH, HFI_H264_LEVEL_41},
	.num_pl = 5,
	.fmts[0] = {HFI_BUFFER_OUTPUT, HFI_COLOR_FORMAT_NV12_UBWC},
	.fmts[1] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12_UBWC},
	.fmts[2] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12},
	.fmts[3] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV21},
	.num_fmts = 4,
}, {
	.codec = HFI_VIDEO_CODEC_HEVC,
	.domain = VIDC_SESSION_TYPE_DEC,
	.caps[0] = {HFI_CAPABILITY_FRAME_WIDTH, 128, 1920, 1},
	.caps[1] = {HFI_CAPABILITY_FRAME_HEIGHT, 128, 1080, 1},
	.caps[2] = {HFI_CAPABILITY_MBS_PER_FRAME, 64, 8160, 1},
	.caps[3] = {HFI_CAPABILITY_BITRATE, 1, 60000000, 1 },
	.caps[4] = {HFI_CAPABILITY_MBS_PER_SECOND, 64, 244800, 1},
	.caps[5] = {HFI_CAPABILITY_FRAMERATE, 1, 120, 1},
	.caps[6] = {HFI_CAPABILITY_MAX_VIDEOCORES, 0, 1, 1},
	.num_caps = 7,
	.pl[0] = {HFI_HEVC_PROFILE_MAIN, HFI_HEVC_LEVEL_5 | HFI_HEVC_TIER_MAIN},
	.pl[1] = {HFI_HEVC_PROFILE_MAIN10, HFI_HEVC_LEVEL_5 | HFI_HEVC_TIER_MAIN},
	.num_pl = 2,
	.fmts[0] = {HFI_BUFFER_OUTPUT, HFI_COLOR_FORMAT_NV12_UBWC},
	.fmts[1] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12_UBWC},
	.fmts[2] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12},
	.fmts[3] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV21},
	.num_fmts = 4,
}, {
	.codec = HFI_VIDEO_CODEC_VP9,
	.domain = VIDC_SESSION_TYPE_DEC,
	.caps[0] = {HFI_CAPABILITY_FRAME_WIDTH, 128, 1920, 1},
	.caps[1] = {HFI_CAPABILITY_FRAME_HEIGHT, 128, 1080, 1},
	.caps[2] = {HFI_CAPABILITY_MBS_PER_FRAME, 64, 8160, 1},
	.caps[3] = {HFI_CAPABILITY_BITRATE, 1, 60000000, 1 },
	.caps[4] = {HFI_CAPABILITY_MBS_PER_SECOND, 64, 244800, 1},
	.caps[5] = {HFI_CAPABILITY_FRAMERATE, 1, 120, 1},
	.caps[6] = {HFI_CAPABILITY_MAX_VIDEOCORES, 0, 1, 1},
	.num_caps = 7,
	.pl[0] = {HFI_VP9_PROFILE_P0, 200},
	.pl[1] = {HFI_VP9_PROFILE_P2_10B, 200},
	.num_pl = 2,
	.fmts[0] = {HFI_BUFFER_OUTPUT, HFI_COLOR_FORMAT_NV12_UBWC},
	.fmts[1] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12_UBWC},
	.fmts[2] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12},
	.fmts[3] = {HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV21},
	.num_fmts = 4,
} };

static const struct hfi_plat_caps *get_capabilities(unsigned int *entries)
{
	*entries = ARRAY_SIZE(caps);
	return caps;
}

static void get_codecs(u32 *enc_codecs, u32 *dec_codecs, u32 *count)
{
	*enc_codecs = 0x0;
	*dec_codecs = HFI_VIDEO_CODEC_H264 | HFI_VIDEO_CODEC_HEVC |
		      HFI_VIDEO_CODEC_VP9;
	*count = 3;
}

static const struct hfi_platform_codec_freq_data codec_freq_data[] = {
	{ V4L2_PIX_FMT_H264, VIDC_SESSION_TYPE_DEC, 440, 0, 440 },
	{ V4L2_PIX_FMT_HEVC, VIDC_SESSION_TYPE_DEC, 440, 0, 440 },
	{ V4L2_PIX_FMT_VP9, VIDC_SESSION_TYPE_DEC, 440, 0, 440 },
};

static const struct hfi_platform_codec_freq_data *
get_codec_freq_data(u32 session_type, u32 pixfmt)
{
	const struct hfi_platform_codec_freq_data *data = codec_freq_data;
	unsigned int i, data_size = ARRAY_SIZE(codec_freq_data);
	const struct hfi_platform_codec_freq_data *found = NULL;

	for (i = 0; i < data_size; i++) {
		if (data[i].pixfmt == pixfmt &&
		    data[i].session_type == session_type) {
			found = &data[i];
			break;
		}
	}

	return found;
}

static unsigned long codec_vpp_freq(u32 session_type, u32 codec)
{
	const struct hfi_platform_codec_freq_data *data;

	data = get_codec_freq_data(session_type, codec);
	if (data)
		return data->vpp_freq;

	return 0;
}

static unsigned long codec_vsp_freq(u32 session_type, u32 codec)
{
	const struct hfi_platform_codec_freq_data *data;

	data = get_codec_freq_data(session_type, codec);
	if (data)
		return data->vsp_freq;

	return 0;
}

static unsigned long codec_lp_freq(u32 session_type, u32 codec)
{
	const struct hfi_platform_codec_freq_data *data;

	data = get_codec_freq_data(session_type, codec);
	if (data)
		return data->low_power_freq;

	return 0;
}

const struct hfi_platform hfi_plat_v6_lite = {
	.codec_vpp_freq = codec_vpp_freq,
	.codec_vsp_freq = codec_vsp_freq,
	.codec_lp_freq = codec_lp_freq,
	.codecs = get_codecs,
	.capabilities = get_capabilities,
	.bufreq = hfi_plat_bufreq_v6,
};
