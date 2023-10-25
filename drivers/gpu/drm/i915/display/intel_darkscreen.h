/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2018 Intel Corporation
 *
 * Author: Nemesa Garg <nemesa.garg@intel.com>
 */

#ifndef __INTEL_DARKSCREEN_H__
#define __INTEL_DARKSCREEN_H__

#include <drm/drm_device.h>

#define DD_COLOR_DEPTH_6BPC 6
#define DD_COLOR_DEPTH_8BPC 8
#define DD_COLOR_DEPTH_10BPC 10
#define DD_COLOR_DEPTH_12BPC 12

// HW Darkscreen Detection Macros
#define DARKSCREEN_PROGRAMMED_COMPARE_VALUE_CALCULATION_FACTOR 12

// Compare Value = 16*(2 ^ (bpc-8))
#define DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_6_BPC 4
#define DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_8_BPC 16
#define DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_10_BPC 64
#define DARKSCREEN_COMPARE_VALUE_LIMITED_RANGE_12_BPC 256

struct intel_crtc_state;
struct intel_crtc;

struct intel_darkscreen {
	bool enable;
	u64 timer_value;
	u8 bpc;
	struct hrtimer timer;
};

void dark_screen_enable(struct intel_crtc_state *crtc_state);
void intel_darkscreen_crtc_debugfs_add(struct intel_crtc *crtc);

#endif /* __INTEL_DARKSCREEN_H_ */
