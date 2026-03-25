// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "i915_vma.h"
#include "intel_writeback_helper.h"

u32 intel_get_ggtt_addr(struct i915_vma *vma)
{
	return i915_ggtt_offset(vma);
}
