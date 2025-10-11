// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_sriov.h"
#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_gt_sriov_pf_control.h"
#include "xe_gt_sriov_pf_migration.h"
#include "xe_pm.h"
#include "xe_sriov_pf_helpers.h"
#include "xe_sriov_pf_migration.h"
#include "xe_sriov_pf_migration_data.h"
#include "xe_sriov_printk.h"

static struct xe_sriov_pf_migration *pf_pick_migration(struct xe_device *xe, unsigned int vfid)
{
	xe_assert(xe, IS_SRIOV_PF(xe));
	xe_assert(xe, vfid <= xe_sriov_pf_get_totalvfs(xe));

	return &xe->sriov.pf.vfs[vfid].migration;
}

/**
 * xe_sriov_pf_migration_waitqueue - Get waitqueue for migration
 * @xe: the &struct xe_device
 * @vfid: the VF identifier
 *
 * Return: pointer to the migration waitqueue.
 */
wait_queue_head_t *xe_sriov_pf_migration_waitqueue(struct xe_device *xe, unsigned int vfid)
{
	return &pf_pick_migration(xe, vfid)->wq;
}

/**
 * xe_sriov_pf_migration_supported() - Check if SR-IOV VF migration is supported by the device
 * @xe: the &struct xe_device
 *
 * Return: true if migration is supported, false otherwise
 */
bool xe_sriov_pf_migration_supported(struct xe_device *xe)
{
	xe_assert(xe, IS_SRIOV_PF(xe));

	return xe->sriov.pf.migration.supported;
}

static bool pf_check_migration_support(struct xe_device *xe)
{
	xe_assert(xe, IS_SRIOV_PF(xe));

	/* XXX: for now this is for feature enabling only */
	return IS_ENABLED(CONFIG_DRM_XE_DEBUG);
}

static void pf_migration_cleanup(struct drm_device *dev, void *arg)
{
	struct xe_sriov_pf_migration *migration = arg;

	if (!IS_ERR_OR_NULL(migration->pending))
		xe_sriov_pf_migration_data_free(migration->pending);
	if (!IS_ERR_OR_NULL(migration->trailer))
		xe_sriov_pf_migration_data_free(migration->trailer);
	if (!IS_ERR_OR_NULL(migration->descriptor))
		xe_sriov_pf_migration_data_free(migration->descriptor);
}

/**
 * xe_sriov_pf_migration_init() - Initialize support for SR-IOV VF migration.
 * @xe: the &struct xe_device
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_pf_migration_init(struct xe_device *xe)
{
	unsigned int n, totalvfs;
	int err;

	xe_assert(xe, IS_SRIOV_PF(xe));

	xe->sriov.pf.migration.supported = pf_check_migration_support(xe);
	if (!xe_sriov_pf_migration_supported(xe))
		return 0;

	totalvfs = xe_sriov_pf_get_totalvfs(xe);
	for (n = 1; n <= totalvfs; n++) {
		struct xe_sriov_pf_migration *migration = pf_pick_migration(xe, n);

		err = drmm_mutex_init(&xe->drm, &migration->lock);
		if (err)
			return err;

		init_waitqueue_head(&migration->wq);

		err = drmm_add_action_or_reset(&xe->drm, pf_migration_cleanup, migration);
		if (err)
			return err;
	}

	return 0;
}

static bool pf_migration_empty(struct xe_device *xe, unsigned int vfid)
{
	struct xe_gt *gt;
	u8 gt_id;

	for_each_gt(gt, xe, gt_id) {
		if (!xe_gt_sriov_pf_migration_ring_empty(gt, vfid))
			return false;
	}

	return true;
}

static struct xe_sriov_pf_migration_data *
pf_migration_consume(struct xe_device *xe, unsigned int vfid)
{
	struct xe_sriov_pf_migration_data *data;
	struct xe_gt *gt;
	u8 gt_id;
	bool no_data = true;

	for_each_gt(gt, xe, gt_id) {
		data = xe_gt_sriov_pf_migration_ring_consume_nowait(gt, vfid);

		if (!IS_ERR(data))
			return data;
		else if (PTR_ERR(data) == -EAGAIN)
			no_data = false;
	}

	if (no_data)
		return ERR_PTR(-ENODATA);

	return ERR_PTR(-EAGAIN);
}

/**
 * xe_sriov_pf_migration_consume() - Consume a SR-IOV VF migration data packet from the device
 * @xe: the &struct xe_device
 * @vfid: the VF identifier
 *
 * If there is no migration data to process, wait until more data is available.
 *
 * Return: Pointer to &struct xe_sriov_pf_migration_data on success,
 *	   ERR_PTR(-ENODATA) if ring is empty and no more migration data is expected,
 *	   ERR_PTR value in case of error.
 *
 * Return: 0 on success or a negative error code on failure.
 */
struct xe_sriov_pf_migration_data *
xe_sriov_pf_migration_consume(struct xe_device *xe, unsigned int vfid)
{
	struct xe_sriov_pf_migration *migration = pf_pick_migration(xe, vfid);
	unsigned long timeout = HZ * 5;
	struct xe_sriov_pf_migration_data *data;
	int ret;

	if (!IS_SRIOV_PF(xe))
		return ERR_PTR(-ENODEV);

	while (1) {
		data = pf_migration_consume(xe, vfid);
		if (!IS_ERR(data) || PTR_ERR(data) != -EAGAIN)
			goto out;

		ret = wait_event_interruptible_timeout(migration->wq,
						       !pf_migration_empty(xe, vfid),
						       timeout);
		if (ret == 0) {
			xe_sriov_warn(xe, "VF%d Timed out waiting for migration data\n", vfid);
			return ERR_PTR(-ETIMEDOUT);
		}

		timeout = ret;
	}

out:
	return data;
}

static int pf_handle_descriptor(struct xe_device *xe, unsigned int vfid,
				struct xe_sriov_pf_migration_data *data)
{
	int ret;

	if (data->tile != 0 || data->gt != 0)
		return -EINVAL;

	ret = xe_sriov_pf_migration_data_process_desc(xe, vfid, data);
	if (ret)
		return ret;

	return 0;
}

static int pf_handle_trailer(struct xe_device *xe, unsigned int vfid,
			     struct xe_sriov_pf_migration_data *data)
{
	struct xe_gt *gt;
	u8 gt_id;

	if (data->tile != 0 || data->gt != 0)
		return -EINVAL;
	if (data->offset != 0 || data->size != 0 || data->buff || data->bo)
		return -EINVAL;

	xe_sriov_pf_migration_data_free(data);

	for_each_gt(gt, xe, gt_id)
		xe_gt_sriov_pf_control_vf_data_eof(gt, vfid);

	return 0;
}

/**
 * xe_sriov_pf_migration_produce() - Produce a SR-IOV VF migration data packet for device to process
 * @xe: the &struct xe_device
 * @vfid: the VF identifier
 * @data: VF migration data
 *
 * If the underlying data structure is full, wait until there is space.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_sriov_pf_migration_produce(struct xe_device *xe, unsigned int vfid,
				  struct xe_sriov_pf_migration_data *data)
{
	struct xe_gt *gt;

	if (!IS_SRIOV_PF(xe))
		return -ENODEV;

	if (data->type == XE_SRIOV_MIG_DATA_DESCRIPTOR)
		return pf_handle_descriptor(xe, vfid, data);
	else if (data->type == XE_SRIOV_MIG_DATA_TRAILER)
		return pf_handle_trailer(xe, vfid, data);

	gt = xe_device_get_gt(xe, data->gt);
	if (!gt || data->tile != gt->tile->id) {
		xe_sriov_err_ratelimited(xe, "VF%d Unknown GT - tile_id:%d, gt_id:%d\n",
					 vfid, data->tile, data->gt);
		return -EINVAL;
	}

	return xe_gt_sriov_pf_migration_ring_produce(gt, vfid, data);
}

/**
 * xe_sriov_pf_migration_size() - Total size of migration data from all components within a device
 * @xe: the &struct xe_device
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: total migration data size in bytes or a negative error code on failure.
 */
ssize_t xe_sriov_pf_migration_size(struct xe_device *xe, unsigned int vfid)
{
	size_t size = 0;
	struct xe_gt *gt;
	ssize_t ret;
	u8 gt_id;

	xe_assert(xe, IS_SRIOV_PF(xe));

	for_each_gt(gt, xe, gt_id) {
		ret = xe_gt_sriov_pf_migration_size(gt, vfid);
		if (ret < 0) {
			size = ret;
			break;
		}
		size += ret;
	}

	return size;
}
