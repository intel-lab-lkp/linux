// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i915_drv.h"
#include "i915_utils.h"

bool i915_vtd_active(struct drm_i915_private *i915)
{
	if (device_iommu_mapped(i915->drm.dev))
		return true;

	/* Running as a guest, we assume the host is enforcing VT'd */
	return i915_run_as_guest();
}
