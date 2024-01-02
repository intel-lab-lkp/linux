/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef __INTEL_PLANE_CAPS_H__
#define __INTEL_PLANE_CAPS_H__

#include "intel_display_types.h"

u8 skl_get_plane_caps(struct drm_i915_private *i915,
		      enum pipe pipe, enum plane_id plane_id);

#endif /* __INTEL_PLANE_CAPS_H__ */
