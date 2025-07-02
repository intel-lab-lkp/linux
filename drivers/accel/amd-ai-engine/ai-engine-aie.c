// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine driver AIE device specific implementation
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/io.h>

#include "ai-engine-internal.h"

#define AIE_ARRAY_SHIFT		30U
#define AIE_COL_SHIFT		23U
#define AIE_ROW_SHIFT		18U

/*
 * Register offsets
 */
#define AIE_SHIMPL_CLKCNTR_REGOFF		0x00036040U
#define AIE_TILE_CORE_CLKCNTR_REGOFF		0x00036040U

/*
 * Register masks
 */
#define AIE_SHIMPL_CLKCNTR_COLBUF_MASK		BIT(0)
#define AIE_SHIMPL_CLKCNTR_NEXTCLK_MASK		BIT(1)
#define AIE_TILE_CLKCNTR_COLBUF_MASK		BIT(0)
#define AIE_TILE_CLKCNTR_NEXTCLK_MASK		BIT(1)

static u32 aie_get_tile_type(struct aie_device *adev, struct aie_location *loc)
{
	if (loc->row)
		return AIE_TILE_TYPE_TILE;
	/* SHIM row */
	if ((loc->col % 4) < 2)
		return AIE_TILE_TYPE_SHIMPL;

	return AIE_TILE_TYPE_SHIMNOC;
}

/* aie_scan_part_clocks() - scan clocks of a partition
 * @apart: AI engine partition
 *
 * Return: 0 for success, negative value for errors.
 */
static int aie_scan_part_clocks(struct aie_partition *apart)
{
	struct aie_aperture *aperture = apart->aperture;
	struct aie_range *range = &apart->range;
	struct aie_device *adev = apart->adev;
	struct aie_location loc;
	int ret;

	/* Clear the bitmap of cores and memories clock state */
	aie_resource_put_region(&apart->cores_clk_state, 0,
				apart->cores_clk_state.total);

	for (loc.col = range->start.col;
	     loc.col < range->start.col + range->size.col;
	     loc.col++) {
		for (loc.row = range->start.row;
		     loc.row < range->start.row + range->size.row - 1;
		     loc.row++) {
			void __iomem *va;
			u32 val, nbitpos;

			/*
			 * Reading registers of the current tile to see the next
			 * tile is clock gated.
			 */
			nbitpos = (loc.col - range->start.col) *
				  (range->size.row - 1) + loc.row;

			if (aie_get_tile_type(adev, &loc) !=
					AIE_TILE_TYPE_TILE) {
				/* Checks shim tile for next core tile */
				va = aperture->base +
				     aie_cal_regoff(adev, loc,
						    AIE_SHIMPL_CLKCNTR_REGOFF);
				val = readl(va);

				/*
				 * check if the clock buffer and the next clock
				 * tile is set, if one of them is not set, the
				 * tiles of the column are clock gated.
				 */
				if (!(val & AIE_SHIMPL_CLKCNTR_COLBUF_MASK) ||
				    !(val & AIE_SHIMPL_CLKCNTR_NEXTCLK_MASK))
					break;

				/* Set next tile in the row clock state on */
				ret = aie_resource_set(&apart->cores_clk_state,
						       nbitpos, 1);
				if (ret) {
					dev_err(aperture->dev,
						"failed to set clock state bitmap.");
					return ret;
				}
				continue;
			}

			/* Checks core tile for next tile */
			va = aperture->base +
			     aie_cal_regoff(adev, loc,
					    AIE_TILE_CORE_CLKCNTR_REGOFF);
			val = readl(va);

			/*
			 * If the next tile is gated, skip the rest of the
			 * column.
			 */
			if (!(val & AIE_TILE_CLKCNTR_NEXTCLK_MASK))
				break;

			ret = aie_resource_set(&apart->cores_clk_state,
					       nbitpos, 1);
			if (ret) {
				dev_err(aperture->dev,
					"failed to set clock state bitmap.");
				return ret;
			}
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

/* aie_set_col_clocks() - set clocks of a range of tiles of a column
 * @apart: AI engine partition
 * @range: range of tiles of a column
 * @enable: true to enable the clock, false to disable
 *
 * Return: 0 for success, negative value for errors.
 */
static int aie_set_col_clocks(struct aie_partition *apart,
			      struct aie_range *range, bool enable)
{
	struct aie_location ploc;
	u32 startbit;
	int ret;

	/*
	 * check if the range is of single colum. only single column is allowed.
	 * check if the start row is tile row, only tile rows are allowed.
	 */
	if (range->size.col != 1 || range->start.row < 1)
		return -EINVAL;

	ploc.col = range->start.col;
	for (ploc.row = range->start.row - 1;
	     ploc.row < range->start.row + range->size.row;
	     ploc.row++) {
		struct aie_aperture *aperture = apart->aperture;
		struct aie_device *adev = apart->adev;
		void __iomem *va;
		u32 val = 0, regoff;

		if (!ploc.row) {
			if (enable)
				val = AIE_SHIMPL_CLKCNTR_COLBUF_MASK |
				      AIE_SHIMPL_CLKCNTR_NEXTCLK_MASK;
			regoff = AIE_SHIMPL_CLKCNTR_REGOFF;
		} else {
			if (enable)
				val = AIE_TILE_CLKCNTR_COLBUF_MASK |
				      AIE_TILE_CLKCNTR_NEXTCLK_MASK;
			regoff = AIE_TILE_CORE_CLKCNTR_REGOFF;
		}

		va = aperture->base + aie_cal_regoff(adev, ploc, regoff);
		writel(val, va);

		/* If the tile clock is not on, no need to go to next row */
		if (!enable)
			break;
	}

	/* Update clock state bitmap */
	startbit = (range->start.col - apart->range.start.col) *
		   (apart->range.size.row - 1) + range->start.row - 1;
	if (enable)
		ret = aie_resource_set(&apart->cores_clk_state, startbit,
				       range->size.row);
	else
		ret = aie_resource_clear(&apart->cores_clk_state, startbit,
					 range->size.row);

	return ret;
}

/* aie_set_part_clocks() - set clocks of a partition
 * @apart: AI engine partition
 *
 * Return: 0 for success, negative value for errors.
 */
static int aie_set_part_clocks(struct aie_partition *apart)
{
	struct aie_aperture *aperture = apart->aperture;
	struct aie_range *range = &apart->range, lrange;
	struct aie_location rloc;
	int ret = 0;

	/*
	 * The tiles below the highest tile whose clock is on, need to have the
	 * clock on. The first for loop is to scan the clock states bitmap to
	 * see which tiles are required to be clocked on, and update the bitmap
	 * to make sure the tiles below are also required to be clocked on.
	 */
	for (rloc.col = 0; rloc.col < range->size.col; rloc.col++) {
		u32 startbit, inuse_toprow = 0, clk_toprow = 0;

		startbit = rloc.col * (range->size.row - 1);

		for (rloc.row = range->start.row + 1;
		     rloc.row < range->start.row + range->size.row;
		     rloc.row++) {
			u32 bit = startbit + rloc.row - 1;

			if (aie_resource_testbit(&apart->tiles_inuse, bit))
				inuse_toprow = rloc.row;
			if (aie_resource_testbit(&apart->cores_clk_state, bit))
				clk_toprow = rloc.row;
		}

		/* Update clock states of a column */
		lrange.start.col = rloc.col + range->start.col;
		lrange.size.col = 1;
		if (inuse_toprow < clk_toprow) {
			lrange.start.row = inuse_toprow + 1;
			lrange.size.row = clk_toprow - inuse_toprow;
			ret = aie_set_col_clocks(apart, &lrange, false);
		} else if (inuse_toprow > clk_toprow) {
			lrange.start.row = clk_toprow + 1;
			lrange.size.row = inuse_toprow - clk_toprow;
			ret = aie_set_col_clocks(apart, &lrange, true);
		}

		if (ret) {
			dev_err(aperture->dev,
				"failed to set clocks for column %u.",
				rloc.col);
			return ret;
		}
	}

	return 0;
}
static const struct aie_tile_operations aie_ops = {
	.get_tile_type = aie_get_tile_type,
	.scan_part_clocks = aie_scan_part_clocks,
	.set_part_clocks = aie_set_part_clocks,
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
