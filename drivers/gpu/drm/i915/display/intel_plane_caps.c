// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_fb.h"
#include "intel_plane_caps.h"

static bool skl_plane_has_rc_ccs(struct drm_i915_private *i915,
				 enum pipe pipe, enum plane_id plane_id)
{
	/* Wa_22011186057 */
	if (IS_ALDERLAKE_P(i915) && IS_DISPLAY_STEP(i915, STEP_A0, STEP_B0))
		return false;

	if (DISPLAY_VER(i915) >= 11)
		return true;

	if (IS_GEMINILAKE(i915))
		return pipe != PIPE_C;

	return pipe != PIPE_C &&
		(plane_id == PLANE_PRIMARY ||
		 plane_id == PLANE_SPRITE0);
}

static bool gen12_plane_has_mc_ccs(struct drm_i915_private *i915,
				   enum plane_id plane_id)
{
	if (DISPLAY_VER(i915) < 12)
		return false;

	/* Wa_14010477008 */
	if (IS_DG1(i915) || IS_ROCKETLAKE(i915) ||
	    (IS_TIGERLAKE(i915) && IS_DISPLAY_STEP(i915, STEP_A0, STEP_D0)))
		return false;

	/* Wa_22011186057 */
	if (IS_ALDERLAKE_P(i915) && IS_DISPLAY_STEP(i915, STEP_A0, STEP_B0))
		return false;

	return plane_id < PLANE_SPRITE4;
}

u8 skl_get_plane_caps(struct drm_i915_private *i915,
		      enum pipe pipe, enum plane_id plane_id)
{
	u8 caps = INTEL_PLANE_CAP_TILING_X;

	if (DISPLAY_VER(i915) < 13 || IS_ALDERLAKE_P(i915))
		caps |= INTEL_PLANE_CAP_TILING_Y;
	if (DISPLAY_VER(i915) < 12)
		caps |= INTEL_PLANE_CAP_TILING_Yf;
	if (HAS_4TILE(i915))
		caps |= INTEL_PLANE_CAP_TILING_4;

	if (skl_plane_has_rc_ccs(i915, pipe, plane_id)) {
		caps |= INTEL_PLANE_CAP_CCS_RC;
		if (DISPLAY_VER(i915) >= 12)
			caps |= INTEL_PLANE_CAP_CCS_RC_CC;
	}

	if (gen12_plane_has_mc_ccs(i915, plane_id))
		caps |= INTEL_PLANE_CAP_CCS_MC;

	return caps;
}
