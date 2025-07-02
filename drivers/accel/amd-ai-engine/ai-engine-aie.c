// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine driver AIE device specific implementation
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>

#include "ai-engine-internal.h"

#define AIE_ARRAY_SHIFT		30U
#define AIE_COL_SHIFT		23U
#define AIE_ROW_SHIFT		18U

static u32 aie_get_tile_type(struct aie_device *adev, struct aie_location *loc)
{
	if (loc->row)
		return AIE_TILE_TYPE_TILE;
	/* SHIM row */
	if ((loc->col % 4) < 2)
		return AIE_TILE_TYPE_SHIMPL;

	return AIE_TILE_TYPE_SHIMNOC;
}

static const struct aie_tile_operations aie_ops = {
	.get_tile_type = aie_get_tile_type,
};

/**
 * aie_device_init() - Initialize AI engine device struct AIE specific
 * @adev: AI engine device
 *
 * This function initialize the AI engine device structure device version
 * specific elements such as register addressing related array shift,
 * column shift, and row shift; AIE device specific device operations, device
 * columns resource.
 */
void aie_device_init(struct aie_device *adev)
{
	adev->array_shift = AIE_ARRAY_SHIFT;
	adev->col_shift = AIE_COL_SHIFT;
	adev->row_shift = AIE_ROW_SHIFT;
	adev->ops = &aie_ops;
}
