/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef __I915_MMIO_RANGE_H__
#define __I915_MMIO_RANGE_H__

#include <linux/types.h>

/* Other register ranges (e.g., shadow tables, MCR tables, etc.) */
struct i915_range {
	u32 start;
	u32 end;
};

bool reg_in_i915_range_table(u32 addr, const struct i915_range *table);

#endif /* __I915_MMIO_RANGE_H__ */
