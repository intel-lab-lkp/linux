// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/minmax.h>

#include <drm/drm_print.h>

#include "intel_display_core.h"
#include "intel_mchbar.h"
#include "intel_mchbar_regs.h"
#include "intel_uncore.h"

static bool has_mchbar_mirror(struct intel_display *display)
{
	return DISPLAY_VER(display) < 14;
}

static u32 mchbar_mirror_base(struct intel_display *display)
{
	if (DISPLAY_VER(display) >= 6)
		return MCHBAR_MIRROR_BASE_SNB;
	else
		return MCHBAR_MIRROR_BASE;
}

static u32 mchbar_mirror_end(struct intel_display *display)
{
	if (DISPLAY_VER(display) >= 12 && !display->platform.rocketlake)
		return MCHBAR_MIRROR_END_TGL;
	else if (DISPLAY_VER(display) >= 11)
		return MCHBAR_MIRROR_END_ICL_RKL;
	else if (DISPLAY_VER(display) >= 6)
		return MCHBAR_MIRROR_END_SNB;
	else
		return MCHBAR_MIRROR_END;
}

static u32 mchbar_mirror_len(struct intel_display *display)
{
	return mchbar_mirror_end(display) - mchbar_mirror_base(display) + 1;
}

static bool is_mchbar_reg(struct intel_display *display, i915_reg_t reg)
{
	return has_mchbar_mirror(display) &&
		in_range32(i915_mmio_reg_offset(reg),
			   mchbar_mirror_base(display),
			   mchbar_mirror_len(display));
}

u16 intel_mchbar_read16(struct intel_display *display,
			i915_reg_t reg)
{
	struct intel_uncore *uncore = to_intel_uncore(display->drm);

	drm_WARN_ON(display->drm, !is_mchbar_reg(display, reg));

	return intel_uncore_read16(uncore, reg);
}

u32 intel_mchbar_read(struct intel_display *display,
		      i915_reg_t reg)
{
	struct intel_uncore *uncore = to_intel_uncore(display->drm);

	drm_WARN_ON(display->drm, !is_mchbar_reg(display, reg));

	return intel_uncore_read(uncore, reg);
}

u64 intel_mchbar_read64(struct intel_display *display,
			i915_reg_t reg)
{
	struct intel_uncore *uncore = to_intel_uncore(display->drm);

	drm_WARN_ON(display->drm, !is_mchbar_reg(display, reg));

	return intel_uncore_read64(uncore, reg);
}
