// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include "xe_ggtt.h"
#include "xe_display_vma.h"
#include "intel_writeback_helper.h"

u32 intel_get_ggtt_addr(struct i915_vma *vma)
{
	return lower_32_bits(xe_ggtt_node_addr(vma->node));
}
