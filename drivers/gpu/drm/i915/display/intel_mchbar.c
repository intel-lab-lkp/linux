// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "intel_display_core.h"
#include "intel_mchbar.h"
#include "intel_uncore.h"

u16 intel_mchbar_read16(struct intel_display *display,
			i915_reg_t reg)
{
	struct intel_uncore *uncore = to_intel_uncore(display->drm);

	return intel_uncore_read16(uncore, reg);
}

u32 intel_mchbar_read(struct intel_display *display,
		      i915_reg_t reg)
{
	struct intel_uncore *uncore = to_intel_uncore(display->drm);

	return intel_uncore_read(uncore, reg);
}

u64 intel_mchbar_read64(struct intel_display *display,
			i915_reg_t reg)
{
	struct intel_uncore *uncore = to_intel_uncore(display->drm);

	return intel_uncore_read64(uncore, reg);
}
