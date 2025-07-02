// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine partition driver
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "ai-engine-internal.h"

/**
 * aie_part_create_mems_info() - creates array to store the AI engine partition
 *				 different memories types information
 * @apart: AI engine partition
 *
 * Return: 0 for success, negative value for failure
 *
 * This function will create array to store the information of different
 * memories types in the partition. This array is stored in @apart->pmems.
 */
static int aie_part_create_mems_info(struct aie_partition *apart)
{
	unsigned int i, num_mems;

	num_mems = apart->adev->ops->get_mem_info(apart->adev, &apart->range,
						  NULL);
	if (!num_mems)
		return 0;

	apart->pmems = devm_kcalloc(apart->aperture->dev, num_mems,
				    sizeof(struct aie_part_mem),
				    GFP_KERNEL);
	if (!apart->pmems)
		return -ENOMEM;

	apart->adev->ops->get_mem_info(apart->adev, &apart->range,
				       apart->pmems);
	for (i = 0; i < num_mems; i++) {
		struct aie_mem *mem = &apart->pmems[i].mem;

		apart->pmems[i].apart = apart;
		apart->pmems[i].size = mem->size *
				       mem->range.size.col *
				       mem->range.size.row;
	}
	return 0;
}

/**
 * aie_part_release() - release an AI engine partition instance
 * @apart: AI engine partition device
 */
void aie_part_release(struct aie_partition *apart)
{
	struct aie_aperture *aperture = apart->aperture;

	aie_part_set_freq(apart, 0);
	mutex_lock(&aperture->mlock);
	aie_resource_put_region(&aperture->cols_res,
				apart->range.start.col -
				aperture->range.start.col,
				apart->range.size.col);
	aie_resource_uninitialize(&apart->cores_clk_state);
	aie_resource_uninitialize(&apart->tiles_inuse);
	list_del(&apart->node);
	devm_kfree(aperture->dev, apart->pmems);
	devm_kfree(aperture->dev, apart);
	mutex_unlock(&aperture->mlock);
}

/**
 * aie_part_create() - create AI engine partition instance
 * @aperture: AI engine aperture
 * @start_col: start column of AI engine partition
 * @num_col: number of columns of AI engine partition
 *
 * Return: created AI engine partition pointer for success, and PTR_ERR
 *	   for failure.
 *
 * This function creates an AI engine partition instance.
 * It creates AI engine partition, the AI engine partition device and
 * the AI engine partition character device.
 */
struct aie_partition *aie_part_create(struct aie_aperture *aperture,
				      u8 start_col, u8 num_col)
{
	struct aie_partition *apart;
	int ret, num_tiles;

	apart = devm_kzalloc(aperture->dev, sizeof(*apart), GFP_KERNEL);
	if (!apart)
		return ERR_PTR(-ENOMEM);

	apart->aperture = aperture;
	apart->adev = aperture->adev;
	mutex_init(&apart->mlock);
	apart->range.start.col = start_col;
	apart->range.size.col = num_col;
	apart->range.start.row = aperture->range.start.row;
	apart->range.size.row = aperture->range.size.row;

	ret = aie_part_create_mems_info(apart);
	if (ret) {
		dev_err(aperture->dev, "failed to create tile memory information.");
		return ERR_PTR(ret);
	}

	/* SHIM row always enabled so it is not needed in the bitmap */
	num_tiles = apart->range.size.col * (apart->range.size.row - 1);
	ret = aie_resource_initialize(&apart->cores_clk_state, num_tiles);
	if (ret) {
		dev_err(aperture->dev, "failed to initialize clock state resource.");
		return ERR_PTR(ret);
	}

	ret = aie_resource_initialize(&apart->tiles_inuse, num_tiles);
	if (ret) {
		dev_err(aperture->dev, "failed to initialize tiles in use resource.");
		aie_resource_uninitialize(&apart->cores_clk_state);
		return ERR_PTR(ret);
	}

	ret = aie_part_scan_clk_state(apart);
	if (ret) {
		dev_err(aperture->dev, "failed to scan clock state.");
		aie_resource_uninitialize(&apart->cores_clk_state);
		aie_resource_uninitialize(&apart->tiles_inuse);
		return ERR_PTR(ret);
	}

	return apart;
}
