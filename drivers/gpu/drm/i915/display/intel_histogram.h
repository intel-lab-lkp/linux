// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_HISTOGRAM_H__
#define __INTEL_HISTOGRAM_H__

#include <linux/types.h>

struct intel_crtc;
struct intel_display;
enum pipe;

#define HISTOGRAM_BIN_COUNT                    32
#define HISTOGRAM_IET_LENGTH                   33

enum intel_global_hist_status {
	INTEL_HISTOGRAM_ENABLE,
	INTEL_HISTOGRAM_DISABLE,
};

enum intel_global_histogram {
	INTEL_HISTOGRAM,
};

enum intel_global_hist_lut {
	INTEL_HISTOGRAM_PIXEL_FACTOR,
};

int intel_histogram_atomic_check(struct intel_crtc *intel_crtc);
void intel_histogram_irq_handler(struct intel_display *display, enum pipe pipe);
int intel_histogram_update(struct intel_crtc *intel_crtc, bool enable);
int intel_histogram_set_iet_lut(struct intel_crtc *intel_crtc, u32 *data);
int intel_histogram_init(struct intel_crtc *intel_crtc);
void intel_histogram_finish(struct intel_crtc *intel_crtc);

#endif /* __INTEL_HISTOGRAM_H__ */
