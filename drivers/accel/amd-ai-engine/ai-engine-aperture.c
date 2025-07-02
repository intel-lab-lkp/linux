// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine aperture driver
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>
#include <linux/device.h>
#include <linux/firmware/xlnx-zynqmp.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "ai-engine-internal.h"

/**
 * aie_aperture_request_part() - request AI engine partition
 * @aperture: AI engine aperture
 * @req: AI engine partition request arguments
 *
 * Return: partition pointer for success, and error pointer for failure
 */
struct aie_partition *
aie_aperture_request_part(struct aie_aperture *aperture,
			  struct aie_partition_req *req)
{
	u8 start_col, num_col, end_col;
	struct aie_partition *apart;
	int ret;

	start_col = req->start_col;
	num_col = req->num_col;
	if (num_col == 0) {
		start_col = aperture->range.start.col;
		num_col = aperture->range.size.col;
	}

	end_col = start_col + num_col - 1;
	if (start_col < aperture->range.start.col ||
	    end_col >= (aperture->range.start.col + aperture->range.size.col))
		return ERR_PTR(-ERANGE);

	mutex_lock(&aperture->mlock);
	ret = aie_resource_get_region(&aperture->cols_res,
				      start_col - aperture->range.start.col,
				      num_col);
	if (ret != (u32)start_col - aperture->range.start.col) {
		/* Column range returned is not what user requested */
		if (ret > 0)
			aie_resource_put_region(&aperture->cols_res, ret, num_col);
		mutex_unlock(&aperture->mlock);
		return ERR_PTR(-EBUSY);
	}

	apart = aie_part_create(aperture, start_col, num_col);
	if (IS_ERR(apart)) {
		aie_resource_put_region(&aperture->cols_res,
					start_col - aperture->range.start.col,
					num_col);
		mutex_unlock(&aperture->mlock);
		return ERR_PTR(-EINVAL);
	}

	list_add_tail(&apart->node, &aperture->partitions);
	mutex_unlock(&aperture->mlock);
	return apart;
}

int aie_aperture_probe(struct platform_device *pdev)
{
	struct aie_device *adev = dev_get_drvdata(pdev->dev.parent);
	struct aie_aperture *laperture, *aperture;
	struct aie_range *range;
	u32 regs[2];
	int ret;

	aperture = devm_kzalloc(&pdev->dev, sizeof(*aperture), GFP_KERNEL);
	if (!aperture)
		return -ENOMEM;

	platform_set_drvdata(pdev, aperture);
	INIT_LIST_HEAD(&aperture->partitions);
	mutex_init(&aperture->mlock);

	aperture->dev = &pdev->dev;
	range = &aperture->range;
	ret = of_property_read_u32_array(pdev->dev.of_node, "xlnx,columns",
					 regs, ARRAY_SIZE(regs));
	if (ret < 0) {
		dev_err(&pdev->dev,
			"probe %pOF failed, no tiles range information.",
			pdev->dev.of_node);
		return ret;
	}
	range->start.col = regs[0] & aligned_byte_mask(1);
	range->size.col = regs[1] & aligned_byte_mask(1);
	range->start.row = 0;
	range->size.row = adev->ttype_attr[AIE_TILE_TYPE_SHIMPL].num_rows +
			  adev->ttype_attr[AIE_TILE_TYPE_MEMORY].num_rows +
			  adev->ttype_attr[AIE_TILE_TYPE_TILE].num_rows;

	ret = of_property_read_u32_index(pdev->dev.of_node, "xlnx,node-id", 0,
					 &aperture->node_id);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"probe %pOF failed, no aperture node id.",
			pdev->dev.of_node);
		return ret;
	}

	/* Validate the aperture */
	list_for_each_entry(laperture, &adev->apertures, node) {
		u32 start_col, end_col, check_start_col, check_end_col;

		if (laperture->node_id == aperture->node_id) {
			dev_err(&pdev->dev,
				"probe failed, aperture %u exists.",
				aperture->node_id);
			return -EINVAL;
		}

		range = &aperture->range;
		start_col = range->start.col;
		end_col  = start_col + range->size.col - 1;
		check_start_col = laperture->range.start.col;
		check_end_col = check_start_col + laperture->range.size.col - 1;
		if ((start_col >= check_start_col &&
		     start_col <= check_end_col) ||
		    (end_col >= check_start_col &&
		     end_col <= check_end_col)) {
			dev_err(&pdev->dev,
				"probe failed, aperture %x overlaps other aperture.",
				aperture->node_id);
			return -EINVAL;
		}
	}

	/*
	 * Initialize columns resource map to remember which columns have been
	 * assigned. Used for partition management.
	 */
	ret = aie_resource_initialize(&aperture->cols_res,
				      aperture->range.size.col);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialize columns resource.");
		return ret;
	}

	aperture->base = devm_ioremap_resource(&pdev->dev, pdev->resource);
	if (!aperture->base) {
		ret = -ENOMEM;
		goto aie_res_uninit;
	}

	ret = zynqmp_pm_request_node(aperture->node_id,
				     ZYNQMP_PM_CAPABILITY_ACCESS, 0,
				     ZYNQMP_PM_REQUEST_ACK_BLOCKING);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to request node %d", aperture->node_id);
		goto aie_res_uninit;
	}

	dev_set_name(&pdev->dev, "aieaperture_%u_%u", aperture->range.start.col,
		     aperture->range.size.col);
	dev_info(&pdev->dev,
		 "AI engine aperture %s, id 0x%x, cols(%u, %u) aie_tile_rows(%u, %u) memory_tile_rows(%u, %u) is probed successfully.",
		 dev_name(&pdev->dev), aperture->node_id,
		 aperture->range.start.col, aperture->range.size.col,
		 adev->ttype_attr[AIE_TILE_TYPE_TILE].start_row,
		 adev->ttype_attr[AIE_TILE_TYPE_TILE].num_rows,
		 adev->ttype_attr[AIE_TILE_TYPE_MEMORY].start_row,
		 adev->ttype_attr[AIE_TILE_TYPE_MEMORY].num_rows);

	aperture->adev = adev;
	mutex_lock(&adev->mlock);
	list_add_tail(&aperture->node, &adev->apertures);
	mutex_unlock(&adev->mlock);

	return ret;

aie_res_uninit:
	aie_resource_uninitialize(&aperture->cols_res);
	return ret;
}

void aie_aperture_remove(struct platform_device *pdev)
{
	struct aie_aperture *aperture = platform_get_drvdata(pdev);

	aie_resource_uninitialize(&aperture->cols_res);
	zynqmp_pm_release_node(aperture->node_id);
}
