/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_VARIANT_H__
#define __MALI_VPU_VARIANT_H__

#include <linux/types.h>
#include <linux/videodev2.h>

#include "mali-vpu-controls.h"
#include "mali-vpu-formats.h"

/**
 * struct mali_vpu_framesize_caps - Variant-specific frame size capability.
 * @stepwise: Stepwise frame size limits and increments supported by the hardware.
 * @width_default: Default width used when userspace does not request one.
 * @height_default: Default height used when userspace does not request one.
 */
struct mali_vpu_framesize_caps {
	struct v4l2_frmsize_stepwise stepwise;
	u32 width_default;
	u32 height_default;
};

/**
 * struct mali_vpu_format_caps - Pixel/coded format capability tables.
 * @dec_src: List of formats supported on decoder OUTPUT queue (compressed bitstream in).
 * @dec_dst: List of formats supported on decoder CAPTURE queue (decoded raw frames).
 */
struct mali_vpu_format_caps {
	const struct mali_vpu_format_list dec_src;
	const struct mali_vpu_format_list dec_dst;
};

/**
 * struct mali_vpu_control_caps - Control capability tables per role.
 * @dec_ctrls: Array of controls supported by the decoder.
 * @num_dec_ctrls: Number of entries in @dec_ctrls.
 */
struct mali_vpu_control_caps {
	const struct mali_vpu_ctrl_desc *dec_ctrls;
	u32 num_dec_ctrls;
};

/**
 * struct mali_vpu_variant - Variant-specific hardware capability descriptor.
 * @rst_names: Array of reset control names for this variant.
 * @num_rsts: Number of reset controls for this variant.
 * @expected_hw_pid: Expected hardware product ID read from the version register.
 * @fmt_caps: Format capability tables for encode/decode paths.
 * @frmsz_caps: Frame dimension limits and defaults.
 * @ctrl_caps:  Control capability tables for encode/decode paths.
 */
struct mali_vpu_variant {
	const char * const *rst_names;
	unsigned int num_rsts;
	u32 expected_hw_pid;

	struct mali_vpu_format_caps	fmt_caps;
	struct mali_vpu_framesize_caps	frmsz_caps;
	struct mali_vpu_control_caps	ctrl_caps;
};

extern const struct mali_vpu_variant mali_vpu_v76_variant;

#endif /* __MALI_VPU_VARIANT_H__ */
