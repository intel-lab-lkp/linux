// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_sriov_pf_migration_data.h"

static bool data_needs_bo(struct xe_sriov_pf_migration_data *data)
{
	unsigned int type = data->type;

	return type == XE_SRIOV_MIG_DATA_CCS ||
	       type == XE_SRIOV_MIG_DATA_VRAM;
}

/**
 * xe_sriov_pf_migration_data_alloc() - Allocate migration data packet
 * @xe: the &struct xe_device
 *
 * Only allocates the "outer" structure, without initializing the migration
 * data backing storage.
 *
 * Return: Pointer to &struct xe_sriov_pf_migration_data on success,
 *         NULL in case of error.
 */
struct xe_sriov_pf_migration_data *
xe_sriov_pf_migration_data_alloc(struct xe_device *xe)
{
	struct xe_sriov_pf_migration_data *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->xe = xe;
	data->hdr_remaining = sizeof(data->hdr);

	return data;
}

/**
 * xe_sriov_pf_migration_data_free() - Free migration data packet
 * @data: the &struct xe_sriov_pf_migration_data packet
 */
void xe_sriov_pf_migration_data_free(struct xe_sriov_pf_migration_data *data)
{
	if (data_needs_bo(data)) {
		if (data->bo)
			xe_bo_unpin_map_no_vm(data->bo);
	} else {
		if (data->buff)
			kvfree(data->buff);
	}

	kfree(data);
}

static int mig_data_init(struct xe_sriov_pf_migration_data *data)
{
	struct xe_gt *gt = xe_device_get_gt(data->xe, data->gt);

	if (!gt || data->tile != gt->tile->id)
		return -EINVAL;

	if (data->size == 0)
		return 0;

	if (data_needs_bo(data)) {
		struct xe_bo *bo = xe_bo_create_pin_map_novm(data->xe, gt->tile,
							     PAGE_ALIGN(data->size),
							     ttm_bo_type_kernel,
							     XE_BO_FLAG_SYSTEM | XE_BO_FLAG_PINNED,
							     false);
		if (IS_ERR(bo))
			return PTR_ERR(bo);

		data->bo = bo;
		data->vaddr = bo->vmap.vaddr;
	} else {
		void *buff = kvzalloc(data->size, GFP_KERNEL);
		if (!buff)
			return -ENOMEM;

		data->buff = buff;
		data->vaddr = buff;
	}

	return 0;
}

/**
 * xe_sriov_pf_migration_data_init() - Initialize the migration data header and backing storage
 * @data: the &struct xe_sriov_pf_migration_data packet
 * @tile_id: tile identifier
 * @gt_id: GT identifier
 * @type: &enum xe_sriov_pf_migration_data_type
 * @offset: offset of data packet payload (within wider resource)
 * @size: size of data packet payload
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_pf_migration_data_init(struct xe_sriov_pf_migration_data *data, u8 tile_id, u8 gt_id,
				    unsigned int type, loff_t offset, size_t size)
{
	xe_assert(data->xe, type < XE_SRIOV_MIG_DATA_MAX);
	data->version = 1;
	data->type = type;
	data->tile = tile_id;
	data->gt = gt_id;
	data->offset = offset;
	data->size = size;
	data->remaining = size;

	return mig_data_init(data);
}

/**
 * xe_sriov_pf_migration_data_init() - Initialize the migration data backing storage based on header
 * @data: the &struct xe_sriov_pf_migration_data packet
 *
 * Header data is expected to be filled prior to calling this function
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_pf_migration_data_init_from_hdr(struct xe_sriov_pf_migration_data *data)
{
	if (WARN_ON(data->hdr_remaining))
		return -EINVAL;

	data->remaining = data->size;

	return mig_data_init(data);
}
