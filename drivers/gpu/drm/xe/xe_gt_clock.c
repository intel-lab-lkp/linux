// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/math64.h>

#include "xe_gt_clock.h"

#include "display/xe_display.h"
#include "regs/xe_gt_regs.h"
#include "regs/xe_regs.h"
#include "xe_assert.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_macros.h"
#include "xe_mmio.h"

static u32 get_crystal_clock_freq(u32 rpm_config_reg)
{
	const u32 f19_2_mhz = 19200000;
	const u32 f24_mhz = 24000000;
	const u32 f25_mhz = 25000000;
	const u32 f38_4_mhz = 38400000;
	u32 crystal_clock = REG_FIELD_GET(RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_MASK,
					  rpm_config_reg);

	switch (crystal_clock) {
	case RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_24_MHZ:
		return f24_mhz;
	case RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_19_2_MHZ:
		return f19_2_mhz;
	case RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_38_4_MHZ:
		return f38_4_mhz;
	case RPM_CONFIG0_CRYSTAL_CLOCK_FREQ_25_MHZ:
		return f25_mhz;
	default:
		XE_WARN_ON("NOT_POSSIBLE");
		return 0;
	}
}

int xe_gt_clock_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	u32 ctc_reg = xe_mmio_read32(&gt->mmio, CTC_MODE);
	u32 freq = 0;

	/* Assuming gen11+ so assert this assumption is correct */
	xe_gt_assert(gt, GRAPHICS_VER(xe) >= 11);

	/*
	 * Use of the display reference clock to determine GT CS frequency
	 * is only relevant to pre-Xe2 platforms.
	 */
	if (GRAPHICS_VER(xe) < 20 && ctc_reg & CTC_SOURCE_DIVIDE_LOGIC) {
		freq = xe_display_get_refclk(xe);
	} else {
		u32 c0 = xe_mmio_read32(&gt->mmio, RPM_CONFIG0);

		freq = get_crystal_clock_freq(c0);

		/*
		 * Now figure out how the command stream's timestamp
		 * register increments from this frequency (it might
		 * increment only every few clock cycle).
		 */
		freq >>= 3 - REG_FIELD_GET(RPM_CONFIG0_CTC_SHIFT_PARAMETER_MASK, c0);
	}

	gt->info.reference_clock = freq;
	return 0;
}

static u64 div_u64_roundup(u64 n, u32 d)
{
	return div_u64(n + d - 1, d);
}

/**
 * xe_gt_clock_interval_to_ms - Convert sampled GT clock ticks to msec
 *
 * @gt: the &xe_gt
 * @count: count of GT clock ticks
 *
 * Returns: time in msec
 */
u64 xe_gt_clock_interval_to_ms(struct xe_gt *gt, u64 count)
{
	return div_u64_roundup(count * MSEC_PER_SEC, gt->info.reference_clock);
}
