// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_sriov_migration_data.h"
#include "xe_sriov_pf_helpers.h"
#include "xe_sriov_pf_migration.h"
#include "xe_sriov_printk.h"

static struct mutex *pf_migration_mutex(struct xe_device *xe, unsigned int vfid)
{
	xe_assert(xe, IS_SRIOV_PF(xe));
	xe_assert(xe, vfid <= xe_sriov_pf_get_totalvfs(xe));
	return &xe->sriov.pf.vfs[vfid].migration.lock;
}

static struct xe_sriov_migration_data **pf_pick_pending(struct xe_device *xe, unsigned int vfid)
{
	xe_assert(xe, IS_SRIOV_PF(xe));
	xe_assert(xe, vfid <= xe_sriov_pf_get_totalvfs(xe));
	lockdep_assert_held(pf_migration_mutex(xe, vfid));

	return &xe->sriov.pf.vfs[vfid].migration.pending;
}

static struct xe_sriov_migration_data **
pf_pick_descriptor(struct xe_device *xe, unsigned int vfid)
{
	xe_assert(xe, IS_SRIOV_PF(xe));
	xe_assert(xe, vfid <= xe_sriov_pf_get_totalvfs(xe));
	lockdep_assert_held(pf_migration_mutex(xe, vfid));

	return &xe->sriov.pf.vfs[vfid].migration.descriptor;
}

static struct xe_sriov_migration_data **pf_pick_trailer(struct xe_device *xe, unsigned int vfid)
{
	xe_assert(xe, IS_SRIOV_PF(xe));
	xe_assert(xe, vfid <= xe_sriov_pf_get_totalvfs(xe));
	lockdep_assert_held(pf_migration_mutex(xe, vfid));

	return &xe->sriov.pf.vfs[vfid].migration.trailer;
}

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
	if (IS_ERR_OR_NULL(data))
		return;

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

static ssize_t vf_mig_data_hdr_read(struct xe_sriov_migration_data *data,
				    char __user *buf, size_t len)
{
	loff_t offset = sizeof(data->hdr) - data->hdr_remaining;

	if (!data->hdr_remaining)
		return -EINVAL;

	if (len > data->hdr_remaining)
		len = data->hdr_remaining;

	if (copy_to_user(buf, (void *)&data->hdr + offset, len))
		return -EFAULT;

	data->hdr_remaining -= len;

	return len;
}

static ssize_t vf_mig_data_read(struct xe_sriov_migration_data *data,
				char __user *buf, size_t len)
{
	if (len > data->remaining)
		len = data->remaining;

	if (copy_to_user(buf, data->vaddr + (data->size - data->remaining), len))
		return -EFAULT;

	data->remaining -= len;

	return len;
}

static ssize_t __vf_mig_data_read_single(struct xe_sriov_migration_data **data,
					 unsigned int vfid, char __user *buf, size_t len)
{
	ssize_t copied = 0;

	if ((*data)->hdr_remaining)
		copied = vf_mig_data_hdr_read(*data, buf, len);
	else
		copied = vf_mig_data_read(*data, buf, len);

	if ((*data)->remaining == 0 && (*data)->hdr_remaining == 0) {
		xe_sriov_migration_data_free(*data);
		*data = NULL;
	}

	return copied;
}

static struct xe_sriov_migration_data **vf_mig_pick_data(struct xe_device *xe, unsigned int vfid)
{
	struct xe_sriov_migration_data **data;

	data = pf_pick_descriptor(xe, vfid);
	if (*data)
		return data;

	data = pf_pick_pending(xe, vfid);
	if (!*data)
		*data = xe_sriov_pf_migration_save_consume(xe, vfid);
	if (*data)
		return data;

	data = pf_pick_trailer(xe, vfid);
	if (*data)
		return data;

	return ERR_PTR(-ENODATA);
}

static ssize_t vf_mig_data_read_single(struct xe_device *xe, unsigned int vfid,
				       char __user *buf, size_t len)
{
	struct xe_sriov_migration_data **data = vf_mig_pick_data(xe, vfid);

	if (IS_ERR_OR_NULL(data))
		return PTR_ERR(data);

	return __vf_mig_data_read_single(data, vfid, buf, len);
}

/**
 * xe_sriov_migration_data_read() - Read migration data from the device.
 * @xe: the &xe_device
 * @vfid: the VF identifier
 * @buf: start address of userspace buffer
 * @len: requested read size from userspace
 *
 * Return: number of bytes that has been successfully read,
 *	   0 if no more migration data is available,
 *	   -errno on failure.
 */
ssize_t xe_sriov_migration_data_read(struct xe_device *xe, unsigned int vfid,
				     char __user *buf, size_t len)
{
	ssize_t ret, consumed = 0;

	xe_assert(xe, IS_SRIOV_PF(xe));

	scoped_cond_guard(mutex_intr, return -EINTR, pf_migration_mutex(xe, vfid)) {
		while (consumed < len) {
			ret = vf_mig_data_read_single(xe, vfid, buf, len - consumed);
			if (ret == -ENODATA)
				break;
			if (ret < 0)
				return ret;

			consumed += ret;
			buf += ret;
		}
	}

	return consumed;
}

static ssize_t vf_mig_hdr_write(struct xe_sriov_migration_data *data,
				const char __user *buf, size_t len)
{
	loff_t offset = sizeof(data->hdr) - data->hdr_remaining;
	int ret;

	if (len > data->hdr_remaining)
		len = data->hdr_remaining;

	if (copy_from_user((void *)&data->hdr + offset, buf, len))
		return -EFAULT;

	data->hdr_remaining -= len;

	if (!data->hdr_remaining) {
		ret = xe_sriov_migration_data_init_from_hdr(data);
		if (ret)
			return ret;
	}

	return len;
}

static ssize_t vf_mig_data_write(struct xe_sriov_migration_data *data,
				 const char __user *buf, size_t len)
{
	if (len > data->remaining)
		len = data->remaining;

	if (copy_from_user(data->vaddr + (data->size - data->remaining), buf, len))
		return -EFAULT;

	data->remaining -= len;

	return len;
}

static ssize_t vf_mig_data_write_single(struct xe_device *xe, unsigned int vfid,
					const char __user *buf, size_t len)
{
	struct xe_sriov_migration_data **data = pf_pick_pending(xe, vfid);
	int ret;
	ssize_t copied;

	if (IS_ERR_OR_NULL(*data)) {
		*data = xe_sriov_migration_data_alloc(xe);
		if (!*data)
			return -ENOMEM;
	}

	if ((*data)->hdr_remaining)
		copied = vf_mig_hdr_write(*data, buf, len);
	else
		copied = vf_mig_data_write(*data, buf, len);

	if ((*data)->hdr_remaining == 0 && (*data)->remaining == 0) {
		ret = xe_sriov_pf_migration_restore_produce(xe, vfid, *data);
		if (ret) {
			xe_sriov_migration_data_free(*data);
			return ret;
		}

		*data = NULL;
	}

	return copied;
}

/**
 * xe_sriov_migration_data_write() - Write migration data to the device.
 * @xe: the &xe_device
 * @vfid: the VF identifier
 * @buf: start address of userspace buffer
 * @len: requested write size from userspace
 *
 * Return: number of bytes that has been successfully written,
 *	   -errno on failure.
 */
ssize_t xe_sriov_migration_data_write(struct xe_device *xe, unsigned int vfid,
				      const char __user *buf, size_t len)
{
	ssize_t ret, produced = 0;

	xe_assert(xe, IS_SRIOV_PF(xe));

	scoped_cond_guard(mutex_intr, return -EINTR, pf_migration_mutex(xe, vfid)) {
		while (produced < len) {
			ret = vf_mig_data_write_single(xe, vfid, buf, len - produced);
			if (ret < 0)
				return ret;

			produced += ret;
			buf += ret;
		}
	}

	return produced;
}

#define MIGRATION_DESCRIPTOR_DWORDS 0
static size_t pf_descriptor_init(struct xe_device *xe, unsigned int vfid)
{
	struct xe_sriov_migration_data **desc = pf_pick_descriptor(xe, vfid);
	struct xe_sriov_migration_data *data;
	int ret;

	data = xe_sriov_migration_data_alloc(xe);
	if (!data)
		return -ENOMEM;

	ret = xe_sriov_migration_data_init(data, 0, 0, XE_SRIOV_MIGRATION_DATA_TYPE_DESCRIPTOR,
					   0, MIGRATION_DESCRIPTOR_DWORDS * sizeof(u32));
	if (ret) {
		xe_sriov_migration_data_free(data);
		return ret;
	}

	*desc = data;

	return 0;
}

static void pf_pending_init(struct xe_device *xe, unsigned int vfid)
{
	struct xe_sriov_migration_data **data = pf_pick_pending(xe, vfid);

	*data = NULL;
}

#define MIGRATION_TRAILER_SIZE 0
static int pf_trailer_init(struct xe_device *xe, unsigned int vfid)
{
	struct xe_sriov_migration_data **trailer = pf_pick_trailer(xe, vfid);
	struct xe_sriov_migration_data *data;
	int ret;

	data = xe_sriov_migration_data_alloc(xe);
	if (!data)
		return -ENOMEM;

	ret = xe_sriov_migration_data_init(data, 0, 0, XE_SRIOV_MIGRATION_DATA_TYPE_TRAILER,
					   0, MIGRATION_TRAILER_SIZE);
	if (ret) {
		xe_sriov_migration_data_free(data);
		return ret;
	}

	*trailer = data;

	return 0;
}

/**
 * xe_sriov_migration_data_save_init() - Initialize the pending save migration data.
 * @xe: the &xe_device
 * @vfid: the VF identifier
 *
 * Return: 0 on success, -errno on failure.
 */
int xe_sriov_migration_data_save_init(struct xe_device *xe, unsigned int vfid)
{
	int ret;

	scoped_cond_guard(mutex_intr, return -EINTR, pf_migration_mutex(xe, vfid)) {
		ret = pf_descriptor_init(xe, vfid);
		if (ret)
			return ret;

		ret = pf_trailer_init(xe, vfid);
		if (ret)
			return ret;

		pf_pending_init(xe, vfid);
	}

	return 0;
}
