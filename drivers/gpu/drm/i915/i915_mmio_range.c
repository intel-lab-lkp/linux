// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "i915_mmio_range.h"

bool reg_in_i915_range_table(u32 addr, const struct i915_range *table)
{
	while (table->start || table->end) {
		if (addr >= table->start && addr <= table->end)
			return true;

		table++;
	}

	return false;
}
