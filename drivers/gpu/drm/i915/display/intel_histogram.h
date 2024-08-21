// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_HISTOGRAM_H__
#define __INTEL_HISTOGRAM_H__

#include <linux/types.h>

struct intel_crtc;
struct drm_i915_private;
enum pipe;

/* GLOBAL_HIST related registers */
#define _DPST_CTL_A					0x490C0
#define _DPST_CTL_B					0x491C0
#define DPST_CTL(pipe)					_MMIO_PIPE(pipe, _DPST_CTL_A, _DPST_CTL_B)
#define DPST_CTL_IE_HIST_EN				REG_BIT(31)
#define DPST_CTL_RESTORE				REG_BIT(28)
#define DPST_CTL_IE_MODI_TABLE_EN			REG_BIT(27)
#define DPST_CTL_HIST_MODE				REG_BIT(24)
#define DPST_CTL_ENHANCEMENT_MODE_MASK			REG_GENMASK(14, 13)
#define DPST_CTL_EN_MULTIPLICATIVE			REG_FIELD_PREP(DPST_CTL_ENHANCEMENT_MODE_MASK, 2)
#define DPST_CTL_IE_TABLE_VALUE_FORMAT			REG_BIT(15)
#define DPST_CTL_BIN_REG_FUNC_SEL			REG_BIT(11)
#define DPST_CTL_BIN_REG_FUNC_TC			REG_FIELD_PREP(DPST_CTL_BIN_REG_FUNC_SEL, 0)
#define DPST_CTL_BIN_REG_FUNC_IE			REG_FIELD_PREP(DPST_CTL_BIN_REG_FUNC_SEL, 1)
#define DPST_CTL_BIN_REG_MASK				REG_GENMASK(6, 0)
#define DPST_CTL_BIN_REG_CLEAR				REG_FIELD_PREP(DPST_CTL_BIN_REG_MASK, 0)
#define DPST_CTL_IE_TABLE_VALUE_FORMAT_2INT_8FRAC	REG_FIELD_PREP(DPST_CTL_IE_TABLE_VALUE_FORMAT, 1)
#define DPST_CTL_IE_TABLE_VALUE_FORMAT_1INT_9FRAC	REG_FIELD_PREP(DPST_CTL_IE_TABLE_VALUE_FORMAT, 0)
#define DPST_CTL_HIST_MODE_YUV				REG_FIELD_PREP(DPST_CTL_HIST_MODE, 0)
#define DPST_CTL_HIST_MODE_HSV				REG_FIELD_PREP(DPST_CTL_HIST_MODE, 1)

#define _DPST_GUARD_A					0x490C8
#define _DPST_GUARD_B					0x491C8
#define DPST_GUARD(pipe)				_MMIO_PIPE(pipe, _DPST_GUARD_A, _DPST_GUARD_B)
#define DPST_GUARD_HIST_INT_EN				REG_BIT(31)
#define DPST_GUARD_HIST_EVENT_STATUS			REG_BIT(30)
#define DPST_GUARD_INTERRUPT_DELAY_MASK			REG_GENMASK(29, 22)
#define DPST_GUARD_INTERRUPT_DELAY(val)			REG_FIELD_PREP(DPST_GUARD_INTERRUPT_DELAY_MASK, val)
#define DPST_GUARD_THRESHOLD_GB_MASK			REG_GENMASK(21, 0)
#define DPST_GUARD_THRESHOLD_GB(val)			REG_FIELD_PREP(DPST_GUARD_THRESHOLD_GB_MASK, val)

#define _DPST_BIN_A					0x490C4
#define _DPST_BIN_B					0x491C4
#define DPST_BIN(pipe)					_MMIO_PIPE(pipe, _DPST_BIN_A, _DPST_BIN_B)
#define DPST_BIN_DATA_MASK				REG_GENMASK(23, 0)
#define DPST_BIN_BUSY					REG_BIT(31)

#define INTEL_HISTOGRAM_PIPEA			0x90000000
#define INTEL_HISTOGRAM_PIPEB			0x90000002
#define INTEL_HISTOGRAM_EVENT(pipe)		PIPE(pipe, \
						     INTEL_HISTOGRAM_PIPEA, \
						     INTEL_HISTOGRAM_PIPEB)

#define HISTOGRAM_BIN_COUNT			32
#define HISTOGRAM_IET_LENGTH			33

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
void intel_histogram_irq_handler(struct drm_i915_private *i915, enum pipe pipe);
int intel_histogram_update(struct intel_crtc *intel_crtc, bool enable);
int intel_histogram_set_iet_lut(struct intel_crtc *intel_crtc, u32 *data);
int intel_histogram_init(struct intel_crtc *intel_crtc);
void intel_histogram_deinit(struct intel_crtc *intel_crtc);

#endif /* __INTEL_HISTOGRAM_H__ */
