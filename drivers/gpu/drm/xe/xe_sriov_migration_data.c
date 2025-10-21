// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_sriov_migration_data.h"

static bool data_needs_bo(struct xe_sriov_migration_data *data)
{
	return data->type == XE_SRIOV_MIGRATION_DATA_TYPE_VRAM;
}

/**
 * xe_sriov_migration_data() - Allocate migration data packet
 * @xe: the &xe_device
 *
 * Only allocates the "outer" structure, without initializing the migration
 * data backing storage.
 *
 * Return: Pointer to &xe_sriov_migration_data on success,
 *         NULL in case of error.
 */
struct xe_sriov_migration_data *
xe_sriov_migration_data_alloc(struct xe_device *xe)
{
	struct xe_sriov_migration_data *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->xe = xe;
	data->hdr_remaining = sizeof(data->hdr);

	return data;
}

/**
 * xe_sriov_migration_data_free() - Free migration data packet.
 * @data: the &xe_sriov_migration_data packet
 */
void xe_sriov_migration_data_free(struct xe_sriov_migration_data *data)
{
	if (data_needs_bo(data))
		xe_bo_unpin_map_no_vm(data->bo);
	else
		kvfree(data->buff);

	kfree(data);
}

static int mig_data_init(struct xe_sriov_migration_data *data)
{
	struct xe_gt *gt = xe_device_get_gt(data->xe, data->gt);

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

#define XE_SRIOV_MIGRATION_DATA_SUPPORTED_VERSION 1
/**
 * xe_sriov_migration_data_init() - Initialize the migration data header and backing storage.
 * @data: the &xe_sriov_migration_data packet
 * @tile_id: tile identifier
 * @gt_id: GT identifier
 * @type: &xe_sriov_migration_data_type
 * @offset: offset of data packet payload (within wider resource)
 * @size: size of data packet payload
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_migration_data_init(struct xe_sriov_migration_data *data, u8 tile_id, u8 gt_id,
				 enum xe_sriov_migration_data_type type, loff_t offset, size_t size)
{
	data->version = XE_SRIOV_MIGRATION_DATA_SUPPORTED_VERSION;
	data->type = type;
	data->tile = tile_id;
	data->gt = gt_id;
	data->offset = offset;
	data->size = size;
	data->remaining = size;

	return mig_data_init(data);
}

/**
 * xe_sriov_migration_data_init() - Initialize the migration data backing storage based on header.
 * @data: the &xe_sriov_migration_data packet
 *
 * Header data is expected to be filled prior to calling this function.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_migration_data_init_from_hdr(struct xe_sriov_migration_data *data)
{
	if (data->version != XE_SRIOV_MIGRATION_DATA_SUPPORTED_VERSION)
		return -EINVAL;

	data->remaining = data->size;

	return mig_data_init(data);
}
