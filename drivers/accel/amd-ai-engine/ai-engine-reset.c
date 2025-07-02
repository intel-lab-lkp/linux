// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine device driver reset implementation
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/amd-ai-engine.h>
#include <linux/firmware/xlnx-zynqmp.h>
#include <linux/io.h>
#include <linux/mutex.h>

#include "ai-engine-internal.h"

/**
 * aie_part_clear_core_regs_of_tile() - clear registers of aie core
 * @apart: AI engine partition
 * @loc: location of aie tile to clear
 */
static void aie_part_clear_core_regs_of_tile(struct aie_partition *apart,
					     struct aie_location loc)
{
	struct aie_device *adev = apart->adev;
	struct aie_aperture *aperture = apart->aperture;
	const struct aie_core_regs_attr *regs = adev->core_regs;
	u32 i;

	for (i = 0; i < adev->num_core_regs; i++) {
		u32 j, soff, eoff, reg;

		soff = aie_cal_regoff(adev, loc, regs[i].core_regs->soff);
		eoff = aie_cal_regoff(adev, loc, regs[i].core_regs->eoff);

		for (reg = soff; reg <= eoff; reg += AIE_CORE_REGS_STEP) {
			for (j = 0; j < regs[i].width; j++)
				writel(0, aperture->base + reg + j * 4);
		}
	}
}

/**
 * aie_part_clear_core_regs - clear registers of aie core of a partition
 * @apart: AI engine partition
 */
static void aie_part_clear_core_regs(struct aie_partition *apart)
{
	struct aie_range *range = &apart->range;
	u32 c, r;

	/* clear core registers for each tile in the partition */
	for (c = range->start.col; c < range->start.col + range->size.col;
			c++) {
		for (r = range->start.row;
				r < range->start.row + range->size.row; r++) {
			struct aie_location loc;
			u32 ttype;

			loc.row = r;
			loc.col = c;
			ttype = apart->adev->ops->get_tile_type(apart->adev,
								&loc);
			if (ttype == AIE_TILE_TYPE_TILE &&
			    aie_part_check_clk_enable_loc(apart, &loc))
				aie_part_clear_core_regs_of_tile(apart, loc);
		}
	}
}

/**
 * aie_part_clean() - reset and clear AI engine partition
 * @apart: AI engine partition
 *
 * Return: 0 for success and negative value for failure
 *
 * This function will:
 *  * gate all the columns
 *  * reset AI engine partition columns
 *  * reset AI engine shims
 *  * clear the memories
 *  * clear core registers
 *  * gate all the tiles in a partition
 *  * update clock state bitmap
 *
 * This function will not validate the partition, the caller will need to
 * provide a valid AI engine partition.
 */
int aie_part_clean(struct aie_partition *apart)
{
	u32 node_id = apart->adev->pm_node_id;
	int ret;

	mutex_lock(&apart->mlock);
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_DIS_COL_CLK_BUFF);
	if (ret < 0)
		goto exit;

	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_COL_RST |
				      XILINX_AIE_OPS_SHIM_RST);
	if (ret < 0)
		goto exit;

	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_ENB_COL_CLK_BUFF);
	if (ret < 0)
		goto exit;

	apart->adev->ops->mem_clear(apart);
	aie_part_clear_core_regs(apart);
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_DIS_COL_CLK_BUFF);
	if (ret < 0)
		goto exit;

	aie_resource_clear_all(&apart->cores_clk_state);

exit:
	mutex_unlock(&apart->mlock);
	return ret;
}

/**
 * aie_part_init_isolation() - Set isolation boundary of AI engine partition
 * @apart: AI engine partition
 */
static void aie_part_init_isolation(struct aie_partition *apart)
{
	struct aie_range *range = &apart->range;
	u32 c, r;
	u8 dir;

	for (c = range->start.col;
	     c < range->start.col + range->size.col; c++) {
		if (c == range->start.col)
			dir = AIE_ISOLATE_WEST_MASK;
		else if (c == (range->start.col + range->size.col - 1))
			dir = AIE_ISOLATE_EAST_MASK;
		else
			dir = 0;

		for (r = range->start.row;
		     r < range->start.row + range->size.row; r++) {
			struct aie_location loc;

			loc.col = c;
			loc.row = r;
			apart->adev->ops->set_tile_isolation(apart, &loc, dir);
		}
	}
}

/**
 * aie_part_initialize() - AI engine partition initialization
 * @apart: AI engine partition
 * @args: User initialization options
 *
 * Return: 0 for success and negative value for failure
 *
 * This function will:
 * - gate all columns
 * - enable column reset
 * - ungate all columns
 * - disable column reset
 * - reset shim tiles
 * - setup axi mm to raise events
 * - setup partition isolation
 * - zeroize memory
 */
int aie_part_initialize(struct aie_partition *apart,
			struct aie_partition_init_args *args)
{
	u32 node_id = apart->adev->pm_node_id;
	int ret;

	if (!args)
		return -EINVAL;

	mutex_lock(&apart->mlock);

	/* Clear resources */
	aie_resource_clear_all(&apart->tiles_inuse);
	aie_resource_clear_all(&apart->cores_clk_state);

	/* This operation will do first 4 steps of sequence */
	if (args->init_opts & AIE_PART_INIT_OPT_COLUMN_RST) {
		ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
					      apart->range.size.col,
					      XILINX_AIE_OPS_COL_RST);
		if (ret < 0)
			goto exit;
	}

	/* Reset Shims */
	if (args->init_opts & AIE_PART_INIT_OPT_SHIM_RST) {
		ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
					      apart->range.size.col,
					      XILINX_AIE_OPS_SHIM_RST);
		if (ret < 0)
			goto exit;
	}

	/* Setup AXIMM events */
	if (args->init_opts & AIE_PART_INIT_OPT_BLOCK_NOCAXIMMERR) {
		ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
					      apart->range.size.col,
					      XILINX_AIE_OPS_ENB_AXI_MM_ERR_EVENT);
		if (ret < 0)
			goto exit;
	}

	/* Setup partition isolation */
	if (args->init_opts & AIE_PART_INIT_OPT_ISOLATE)
		aie_part_init_isolation(apart);

	/* Zeroize memory */
	if (args->init_opts & AIE_PART_INIT_OPT_ZEROIZEMEM) {
		ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
					      apart->range.size.col,
					      XILINX_AIE_OPS_ZEROISATION);
		if (ret < 0)
			goto exit;
	}

	/* Set L2 interrupt */
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_SET_L2_CTRL_NPI_INTR);
	if (ret < 0)
		goto exit;

	/* Request tile locations */
	ret = aie_part_request_tiles(apart, args->num_tiles, args->locs);

exit:
	mutex_unlock(&apart->mlock);
	return ret;
}

/**
 * aie_part_teardown() - AI engine partition teardown
 * @apart: AI engine partition
 *
 * Return: 0 for success and negative value for failure
 *
 * This function will:
 * - gate all columns
 * - enable column reset
 * - ungate all columns
 * - disable column reset
 * - reset shim tiles
 * - zeroize memory
 * - gate all columns
 */
int aie_part_teardown(struct aie_partition *apart)
{
	u32 node_id = apart->adev->pm_node_id;
	int ret;

	mutex_lock(&apart->mlock);

	/* This operation will do first 4 steps of sequence */
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_COL_RST);
	if (ret < 0)
		goto exit;

	/* Reset shims */
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_SHIM_RST);
	if (ret < 0)
		goto exit;

	/* Zeroize mem */
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_ZEROISATION);
	if (ret < 0)
		goto exit;

	/* Gate all columns */
	ret = zynqmp_pm_aie_operation(node_id, apart->range.start.col,
				      apart->range.size.col,
				      XILINX_AIE_OPS_DIS_COL_CLK_BUFF);
	if (ret < 0)
		goto exit;

	/* Clear tile_inuse bitmap */
	ret = aie_part_release_tiles(apart, 0U, NULL);

exit:
	mutex_unlock(&apart->mlock);
	return ret;
}
