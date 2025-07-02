// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine driver AIEML device specific implementation
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/firmware/xlnx-zynqmp.h>
#include <linux/io.h>

#include "ai-engine-internal.h"

#define AIEML_ARRAY_SHIFT	32U
#define AIEML_COL_SHIFT		25U
#define AIEML_ROW_SHIFT		20U

#define NUM_TYPES_OF_MEM	3U

#define NUM_MODS_CORE_TILE	2U
#define NUM_MODS_MEM_TILE	1U
#define NUM_MODS_SHIMPL_TILE	1U

/*
 * Register offsets
 */
#define AIEML_SHIMPL_COLCLOCK_CTRL_REGOFF		0x000fff20U

/*
 * Register masks
 */
#define AIEML_SHIMPL_COLRESET_CTRL_MASK			GENMASK(1, 0)
#define AIEML_SHIMPL_COLCLOCK_CTRL_MASK			GENMASK(1, 0)

static u32 aieml_get_tile_type(struct aie_device *adev,
			       struct aie_location *loc)
{
	u8 num_mem_rows = adev->ttype_attr[AIE_TILE_TYPE_MEMORY].num_rows;

	if (loc->row > num_mem_rows)
		return AIE_TILE_TYPE_TILE;
	if (loc->row && loc->row <= num_mem_rows)
		return AIE_TILE_TYPE_MEMORY;
	if (loc->row == 0)
		if ((loc->col % 4) < 2)
			return AIE_TILE_TYPE_SHIMPL;

	return AIE_TILE_TYPE_SHIMNOC;
}

/* aieml_scan_part_clocks() - scan clocks of a partition
 * @apart: AI engine partition
 *
 * Return: 0 for success, negative value for errors.
 */
static int aieml_scan_part_clocks(struct aie_partition *apart)
{
	struct aie_aperture *aperture = apart->aperture;
	struct aie_range *range = &apart->range;
	struct aie_device *adev = apart->adev;
	struct aie_location loc;
	int ret;

	/* Clear the bitmap of cores and memories clock state */
	aie_resource_put_region(&apart->cores_clk_state, 0,
				apart->cores_clk_state.total);

	/*
	 * In aieml if clock buffer on shim tile is enabled, the clock for all
	 * tiles in the same column is enabled.
	 */
	for (loc.col = range->start.col;
	     loc.col < range->start.col + range->size.col;
	     loc.col++) {
		void __iomem *va;
		u32 val, nbitpos;

		nbitpos = (loc.col - range->start.col) * (range->size.row - 1);

		va = aperture->base +
		     aie_cal_regoff(adev, loc,
				    AIEML_SHIMPL_COLCLOCK_CTRL_REGOFF);
		val = readl(va);

		if (!(val & AIEML_SHIMPL_COLCLOCK_CTRL_MASK))
			continue;

		ret = aie_resource_set(&apart->cores_clk_state, nbitpos,
				       range->size.row - 1);
		if (ret) {
			dev_err(aperture->dev,
				"failed to set clock state bitmaps for column %u",
				loc.col);
			return ret;
		}
	}
	/*
	 * Set the tiles in use bitmap.
	 * In case of scanning, tiles which are powered on are considered as
	 * tiles in use.
	 */
	bitmap_copy(apart->tiles_inuse.bitmap, apart->cores_clk_state.bitmap,
		    apart->tiles_inuse.total);

	return 0;
}

/* aieml_set_part_clocks() - set clocks of a partition
 * @apart: AI engine partition
 *
 * Return: 0 for success, negative value for errors.
 */
static int aieml_set_part_clocks(struct aie_partition *apart)
{
	struct aie_aperture *aperture = apart->aperture;
	struct aie_range *range = &apart->range;
	u32 node_id = apart->adev->pm_node_id;
	struct aie_location loc;
	int ret;

	for (loc.col = range->start.col;
	     loc.col < range->start.col + range->size.col;
	     loc.col++) {
		u32 startbit, col_inuse = 0;

		startbit = (loc.col - range->start.col) * (range->size.row - 1);

		for (loc.row = range->start.row + 1;
		     loc.row < range->start.row + range->size.row;
		     loc.row++) {
			u32 nbitpos = startbit + loc.row - 1;

			if (aie_resource_testbit(&apart->tiles_inuse, nbitpos)) {
				col_inuse = 1;
				break;
			}
		}

		if (col_inuse) {
			ret = zynqmp_pm_aie_operation(node_id, loc.col,
						      1,
						      XILINX_AIE_OPS_ENB_COL_CLK_BUFF);
			if (ret < 0) {
				dev_err(aperture->dev,
					"failed to enable clock for column: %d",
					loc.col);
				return ret;
			}

			ret = aie_resource_set(&apart->tiles_inuse,
					       startbit, apart->range.size.row - 1) |
			      aie_resource_set(&apart->cores_clk_state,
					       startbit, apart->range.size.row - 1);
			if (ret) {
				dev_err(aperture->dev,
					"failed to set bitmaps for column: %d",
					loc.col);
				return ret;
			}
		} else {
			ret = zynqmp_pm_aie_operation(node_id, loc.col,
						      1,
						      XILINX_AIE_OPS_DIS_COL_CLK_BUFF);
			if (ret < 0) {
				dev_err(aperture->dev,
					"failed to disable clock for column: %d",
					loc.col);
				return ret;
			}

			ret = aie_resource_clear(&apart->tiles_inuse,
						 startbit, apart->range.size.row - 1) |
			      aie_resource_clear(&apart->cores_clk_state,
						 startbit, apart->range.size.row - 1);
			if (ret) {
				dev_err(aperture->dev,
					"failed to clear bitmaps for column: %d",
					loc.col);
				return ret;
			}
		}
	}

	return 0;
}

static const struct aie_tile_operations aieml_ops = {
	.get_tile_type = aieml_get_tile_type,
	.scan_part_clocks = aieml_scan_part_clocks,
	.set_part_clocks = aieml_set_part_clocks,
};

/**
 * aieml_device_init() - Initialize AI engine device struct AIEML specific
 * @adev: AI engine device
 *
 * This function initialize the AI engine device structure device version
 * specific elements such as register addressing related array shift,
 * column shift, and row shift; AIEML device specific device operations, device
 * columns resource.
 */
void aieml_device_init(struct aie_device *adev)
{
	adev->array_shift = AIEML_ARRAY_SHIFT;
	adev->col_shift = AIEML_COL_SHIFT;
	adev->row_shift = AIEML_ROW_SHIFT;
	adev->ops = &aieml_ops;
}
