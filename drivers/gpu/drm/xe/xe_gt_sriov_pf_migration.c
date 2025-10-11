// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <drm/drm_managed.h>

#include "abi/guc_actions_sriov_abi.h"
#include "xe_bo.h"
#include "xe_gt_sriov_pf.h"
#include "xe_gt_sriov_pf_config.h"
#include "xe_gt_sriov_pf_control.h"
#include "xe_gt_sriov_pf_helpers.h"
#include "xe_gt_sriov_pf_migration.h"
#include "xe_gt_sriov_printk.h"
#include "xe_guc_buf.h"
#include "xe_guc_ct.h"
#include "xe_migrate.h"
#include "xe_sriov.h"
#include "xe_sriov_pf_migration.h"
#include "xe_sriov_pf_migration_data.h"

#define XE_GT_SRIOV_PF_MIGRATION_RING_TIMEOUT (HZ * 20)
#define XE_GT_SRIOV_PF_MIGRATION_RING_SIZE 5

static struct xe_gt_sriov_pf_migration *pf_pick_gt_migration(struct xe_gt *gt, unsigned int vfid)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return &gt->sriov.pf.vfs[vfid].migration;
}

static void pf_dump_mig_data(struct xe_gt *gt, unsigned int vfid,
			     struct xe_sriov_pf_migration_data *data)
{
	if (IS_ENABLED(CONFIG_DRM_XE_DEBUG_SRIOV)) {
		print_hex_dump_bytes("mig_data: ", DUMP_PREFIX_OFFSET,
				     data->vaddr, min(SZ_64, data->size));
	}
}

static int pf_save_vf_ggtt_mig_data(struct xe_gt *gt, unsigned int vfid)
{
	struct xe_sriov_pf_migration_data *data;
	size_t size;
	int ret;

	size = xe_gt_sriov_pf_config_get_ggtt(gt, vfid);
	if (size == 0)
		return 0;
	size = size / XE_PAGE_SIZE * sizeof(u64);

	data = xe_sriov_pf_migration_data_alloc(gt_to_xe(gt));
	if (!data)
		return -ENOMEM;

	ret = xe_sriov_pf_migration_data_init(data, gt->tile->id, gt->info.id,
					      XE_SRIOV_MIG_DATA_GGTT, 0, size);
	if (ret)
		goto fail;

	ret = xe_gt_sriov_pf_config_ggtt_save(gt, vfid, data->vaddr, size);
	if (ret)
		goto fail;

	pf_dump_mig_data(gt, vfid, data);

	ret = xe_gt_sriov_pf_migration_ring_produce(gt, vfid, data);
	if (ret)
		goto fail;

	return 0;

fail:
	xe_sriov_pf_migration_data_free(data);
	xe_gt_sriov_err(gt, "Unable to save VF%u GGTT data (%d)\n", vfid, ret);
	return ret;
}

static int pf_restore_vf_ggtt_mig_data(struct xe_gt *gt, unsigned int vfid,
				       struct xe_sriov_pf_migration_data *data)
{
	size_t size;
	int ret;

	size = xe_gt_sriov_pf_config_get_ggtt(gt, vfid) / XE_PAGE_SIZE * sizeof(u64);
	if (size != data->hdr.size)
		return -EINVAL;

	pf_dump_mig_data(gt, vfid, data);

	ret = xe_gt_sriov_pf_config_ggtt_restore(gt, vfid, data->vaddr, size);
	if (ret)
		return ret;

	return 0;
}

/**
 * xe_gt_sriov_pf_migration_ggtt_size() - Get the size of VF GGTT migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: size in bytes or a negative error code on failure.
 */
ssize_t xe_gt_sriov_pf_migration_ggtt_size(struct xe_gt *gt, unsigned int vfid)
{
	if (gt != xe_root_mmio_gt(gt_to_xe(gt)))
		return 0;

	return xe_gt_sriov_pf_config_get_ggtt(gt, vfid) / XE_PAGE_SIZE * sizeof(u64);
}

/**
 * xe_gt_sriov_pf_migration_ggtt_save() - Save VF GGTT migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_ggtt_save(struct xe_gt *gt, unsigned int vfid)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return pf_save_vf_ggtt_mig_data(gt, vfid);
}

/**
 * xe_gt_sriov_pf_migration_ggtt_restore() - Restore VF GGTT migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_ggtt_restore(struct xe_gt *gt, unsigned int vfid,
					  struct xe_sriov_pf_migration_data *data)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return pf_restore_vf_ggtt_mig_data(gt, vfid, data);
}

/* Return: number of dwords saved/restored/required or a negative error code on failure */
static int guc_action_vf_save_restore(struct xe_guc *guc, u32 vfid, u32 opcode,
				      u64 addr, u32 ndwords)
{
	u32 request[PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_LEN] = {
		FIELD_PREP(GUC_HXG_MSG_0_ORIGIN, GUC_HXG_ORIGIN_HOST) |
		FIELD_PREP(GUC_HXG_MSG_0_TYPE, GUC_HXG_TYPE_REQUEST) |
		FIELD_PREP(GUC_HXG_REQUEST_MSG_0_ACTION, GUC_ACTION_PF2GUC_SAVE_RESTORE_VF) |
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_0_OPCODE, opcode),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_1_VFID, vfid),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_2_ADDR_LO, lower_32_bits(addr)),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_3_ADDR_HI, upper_32_bits(addr)),
		FIELD_PREP(PF2GUC_SAVE_RESTORE_VF_REQUEST_MSG_4_SIZE, ndwords),
	};

	return xe_guc_ct_send_block(&guc->ct, request, ARRAY_SIZE(request));
}

/* Return: size of the state in dwords or a negative error code on failure */
static int pf_send_guc_query_vf_mig_data_size(struct xe_gt *gt, unsigned int vfid)
{
	int ret;

	ret = guc_action_vf_save_restore(&gt->uc.guc, vfid, GUC_PF_OPCODE_VF_SAVE, 0, 0);
	return ret ?: -ENODATA;
}

/* Return: number of state dwords saved or a negative error code on failure */
static int pf_send_guc_save_vf_mig_data(struct xe_gt *gt, unsigned int vfid,
					void *dst, size_t size)
{
	const int ndwords = size / sizeof(u32);
	struct xe_guc *guc = &gt->uc.guc;
	CLASS(xe_guc_buf, buf)(&guc->buf, ndwords);
	int ret;

	xe_gt_assert(gt, size % sizeof(u32) == 0);
	xe_gt_assert(gt, size == ndwords * sizeof(u32));

	if (!xe_guc_buf_is_valid(buf))
		return -ENOBUFS;

	memset(xe_guc_buf_cpu_ptr(buf), 0, size);

	ret = guc_action_vf_save_restore(guc, vfid, GUC_PF_OPCODE_VF_SAVE,
					 xe_guc_buf_flush(buf), ndwords);
	if (!ret) {
		ret = -ENODATA;
	} else if (ret > ndwords) {
		ret = -EPROTO;
	} else if (ret > 0) {
		xe_guc_buf_sync(buf);
		memcpy(dst, xe_guc_buf_cpu_ptr(buf), ret * sizeof(u32));
	}

	return ret;
}

/* Return: number of state dwords restored or a negative error code on failure */
static int pf_send_guc_restore_vf_mig_data(struct xe_gt *gt, unsigned int vfid,
					   const void *src, size_t size)
{
	const int ndwords = size / sizeof(u32);
	struct xe_guc *guc = &gt->uc.guc;
	CLASS(xe_guc_buf_from_data, buf)(&guc->buf, src, size);
	int ret;

	xe_gt_assert(gt, size % sizeof(u32) == 0);
	xe_gt_assert(gt, size == ndwords * sizeof(u32));

	if (!xe_guc_buf_is_valid(buf))
		return -ENOBUFS;

	ret = guc_action_vf_save_restore(guc, vfid, GUC_PF_OPCODE_VF_RESTORE,
					 xe_guc_buf_flush(buf), ndwords);
	if (!ret)
		ret = -ENODATA;
	else if (ret > ndwords)
		ret = -EPROTO;

	return ret;
}

static bool pf_migration_supported(struct xe_gt *gt)
{
	return xe_sriov_pf_migration_supported(gt_to_xe(gt));
}

static int pf_save_vf_guc_mig_data(struct xe_gt *gt, unsigned int vfid)
{
	struct xe_sriov_pf_migration_data *data;
	size_t size;
	int ret;

	ret = pf_send_guc_query_vf_mig_data_size(gt, vfid);
	if (ret < 0)
		goto fail;

	size = ret * sizeof(u32);
	xe_gt_sriov_dbg_verbose(gt, "VF%u GuC state size is %d dwords (%zu bytes)\n",
				vfid, ret, size);

	data = xe_sriov_pf_migration_data_alloc(gt_to_xe(gt));
	if (!data) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = xe_sriov_pf_migration_data_init(data, gt->tile->id, gt->info.id,
					      XE_SRIOV_MIG_DATA_GUC, 0, size);
	if (ret)
		goto fail_free;

	ret = pf_send_guc_save_vf_mig_data(gt, vfid, data->vaddr, size);
	if (ret < 0)
		goto fail_free;
	size = ret * sizeof(u32);
	xe_gt_assert(gt, size);
	xe_gt_assert(gt, size <= data->size);
	data->size = size;
	data->remaining = size;

	pf_dump_mig_data(gt, vfid, data);

	ret = xe_gt_sriov_pf_migration_ring_produce(gt, vfid, data);
	if (ret)
		goto fail_free;

	return 0;

fail_free:
	xe_sriov_pf_migration_data_free(data);
fail:
	xe_gt_sriov_err(gt, "Unable to save VF%u GuC data (%pe)\n", vfid, ERR_PTR(ret));
	return ret;
}

/**
 * xe_gt_sriov_pf_migration_guc_size() - Get the size of VF GuC migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: size in bytes or a negative error code on failure.
 */
ssize_t xe_gt_sriov_pf_migration_guc_size(struct xe_gt *gt, unsigned int vfid)
{
	ssize_t size;

	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	if (!pf_migration_supported(gt))
		return -ENOPKG;

	size = pf_send_guc_query_vf_mig_data_size(gt, vfid);
	if (size >= 0)
		size *= sizeof(u32);

	return size;
}

/**
 * xe_gt_sriov_pf_migration_guc_save() - Save VF GuC migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_guc_save(struct xe_gt *gt, unsigned int vfid)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	if (!pf_migration_supported(gt))
		return -ENOPKG;

	return pf_save_vf_guc_mig_data(gt, vfid);
}

static int pf_restore_vf_guc_state(struct xe_gt *gt, unsigned int vfid,
				   struct xe_sriov_pf_migration_data *data)
{
	int ret;

	xe_gt_assert(gt, data->size);

	xe_gt_sriov_dbg_verbose(gt, "restoring %lld dwords of VF%u GuC state\n",
				data->size / sizeof(u32), vfid);
	pf_dump_mig_data(gt, vfid, data);

	ret = pf_send_guc_restore_vf_mig_data(gt, vfid, data->vaddr, data->size);
	if (ret < 0)
		goto fail;

	xe_gt_sriov_dbg_verbose(gt, "restored %d dwords of VF%u GuC state\n", ret, vfid);
	return 0;

fail:
	xe_gt_sriov_dbg(gt, "Failed to restore VF%u GuC state (%pe)\n", vfid, ERR_PTR(ret));
	return ret;
}

/**
 * xe_gt_sriov_pf_migration_guc_restore() - Restore VF GuC migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_guc_restore(struct xe_gt *gt, unsigned int vfid,
					 struct xe_sriov_pf_migration_data *data)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	if (!pf_migration_supported(gt))
		return -ENOPKG;

	return pf_restore_vf_guc_state(gt, vfid, data);
}

/**
 * xe_gt_sriov_pf_migration_mmio_size() - Get the size of VF MMIO migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: size in bytes or a negative error code on failure.
 */
ssize_t xe_gt_sriov_pf_migration_mmio_size(struct xe_gt *gt, unsigned int vfid)
{
	return xe_gt_sriov_pf_mmio_vf_size(gt, vfid);
}

static int pf_save_vf_mmio_mig_data(struct xe_gt *gt, unsigned int vfid)
{
	struct xe_sriov_pf_migration_data *data;
	size_t size;
	int ret;

	size = xe_gt_sriov_pf_migration_mmio_size(gt, vfid);
	if (size == 0)
		return 0;

	data = xe_sriov_pf_migration_data_alloc(gt_to_xe(gt));
	if (!data)
		return -ENOMEM;

	ret = xe_sriov_pf_migration_data_init(data, gt->tile->id, gt->info.id,
					      XE_SRIOV_MIG_DATA_MMIO, 0, size);
	if (ret)
		goto fail;

	ret = xe_gt_sriov_pf_mmio_vf_save(gt, vfid, data->vaddr, size);
	if (ret)
		goto fail;

	pf_dump_mig_data(gt, vfid, data);

	ret = xe_gt_sriov_pf_migration_ring_produce(gt, vfid, data);
	if (ret)
		goto fail;

	return 0;

fail:
	xe_sriov_pf_migration_data_free(data);
	xe_gt_sriov_err(gt, "Unable to save VF%u MMIO data (%d)\n", vfid, ret);
	return ret;
}

static int pf_restore_vf_mmio_mig_data(struct xe_gt *gt, unsigned int vfid,
				       struct xe_sriov_pf_migration_data *data)
{
	pf_dump_mig_data(gt, vfid, data);

	return xe_gt_sriov_pf_mmio_vf_restore(gt, vfid, data->vaddr, data->size);
}

/**
 * xe_gt_sriov_pf_migration_mmio_save() - Save VF MMIO migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_mmio_save(struct xe_gt *gt, unsigned int vfid)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return pf_save_vf_mmio_mig_data(gt, vfid);
}

/**
 * xe_gt_sriov_pf_migration_mmio_restore() - Restore VF MMIO migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_mmio_restore(struct xe_gt *gt, unsigned int vfid,
					  struct xe_sriov_pf_migration_data *data)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return pf_restore_vf_mmio_mig_data(gt, vfid, data);
}

/**
 * xe_gt_sriov_pf_migration_vram_size() - Get the size of VF VRAM migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: size in bytes or a negative error code on failure.
 */
ssize_t xe_gt_sriov_pf_migration_vram_size(struct xe_gt *gt, unsigned int vfid)
{
	if (gt != xe_root_mmio_gt(gt_to_xe(gt)))
		return 0;

	return xe_gt_sriov_pf_config_get_lmem(gt, vfid);
}

static struct dma_fence *__pf_save_restore_vram(struct xe_gt *gt, unsigned int vfid,
						struct xe_bo *vram, u64 vram_offset,
						struct xe_bo *sysmem, u64 sysmem_offset,
						struct xe_bo *ccs, u64 ccs_offset,
						size_t size, bool save)
{
	struct dma_fence *ret = NULL;
	struct drm_exec exec;
	int err;

	xe_gt_assert(gt, sysmem || ccs);

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT, 0);
	drm_exec_until_all_locked(&exec) {
		err = drm_exec_lock_obj(&exec, &vram->ttm.base);
		drm_exec_retry_on_contention(&exec);
		if (err) {
			ret = ERR_PTR(err);
			goto err;
		}

		if (sysmem) {
			err = drm_exec_lock_obj(&exec, &sysmem->ttm.base);
			drm_exec_retry_on_contention(&exec);
			if (err) {
				ret = ERR_PTR(err);
				goto err;
			}
		}

		if (ccs) {
			err = drm_exec_lock_obj(&exec, &ccs->ttm.base);
			drm_exec_retry_on_contention(&exec);
			if (err) {
				ret = ERR_PTR(err);
				goto err;
			}
		}
	}

	ret = xe_migrate_raw_vram_copy(vram, vram_offset,
				       sysmem, sysmem_offset,
				       ccs, ccs_offset,
				       size, save);

err:
	drm_exec_fini(&exec);

	return ret;
}

static int pf_save_vram_chunk(struct xe_gt *gt, unsigned int vfid,
			      struct xe_bo *src_vram, u64 src_vram_offset,
			      size_t size)
{
	struct xe_sriov_pf_migration_data *data;
	struct dma_fence *fence;
	int ret;

	data = xe_sriov_pf_migration_data_alloc(gt_to_xe(gt));
	if (!data)
		return -ENOMEM;

	ret = xe_sriov_pf_migration_data_init(data, gt->tile->id, gt->info.id,
					      XE_SRIOV_MIG_DATA_VRAM, src_vram_offset, size);
	if (ret)
		goto fail;

	fence = __pf_save_restore_vram(gt, vfid,
				       src_vram, src_vram_offset,
				       data->bo, 0,
				       NULL, 0, size, true);

	ret = dma_fence_wait_timeout(fence, false, 5 * HZ);
	dma_fence_put(fence);
	if (!ret) {
		ret = -ETIME;
		goto fail;
	}

	pf_dump_mig_data(gt, vfid, data);

	ret = xe_gt_sriov_pf_migration_ring_produce(gt, vfid, data);
	if (ret)
		goto fail;

	return 0;

fail:
	xe_sriov_pf_migration_data_free(data);
	return ret;
}

#define VF_VRAM_STATE_CHUNK_MAX_SIZE SZ_512M
static int pf_save_vf_vram_mig_data(struct xe_gt *gt, unsigned int vfid)
{
	struct xe_bo *vram;
	loff_t offset = 0;
	size_t size;
	int ret;

	vram = xe_gt_sriov_pf_config_get_lmem_obj(gt, vfid);
	if (!vram)
		return -ENXIO;

	size = xe_bo_size(vram);

	while (size > 0) {
		size_t chunk_size = min(size, VF_VRAM_STATE_CHUNK_MAX_SIZE);

		ret = pf_save_vram_chunk(gt, vfid, vram, offset, chunk_size);
		if (ret)
			goto fail;

		offset += chunk_size;
		size -= chunk_size;
	}

	xe_bo_put(vram);

	return 0;

fail:
	xe_bo_put(vram);
	xe_gt_sriov_err(gt, "Unable to save VF%u VRAM data (%d)\n", vfid, ret);
	return ret;
}

static int pf_restore_vf_vram_mig_data(struct xe_gt *gt, unsigned int vfid,
				       struct xe_sriov_pf_migration_data *data)
{
	u64 end = data->hdr.offset + data->hdr.size;
	struct dma_fence *fence;
	struct xe_bo *vram;
	size_t size;
	int ret = 0;

	vram = xe_gt_sriov_pf_config_get_lmem_obj(gt, vfid);
	if (!vram)
		return -ENXIO;

	size = xe_bo_size(vram);

	if (end > size || end < data->hdr.size) {
		ret = -EINVAL;
		goto err;
	}

	pf_dump_mig_data(gt, vfid, data);

	fence = __pf_save_restore_vram(gt, vfid, vram, data->hdr.offset,
				       data->bo, 0,
				       NULL, 0, data->hdr.size, false);
	ret = dma_fence_wait_timeout(fence, false, 5 * HZ);
	dma_fence_put(fence);
	if (!ret) {
		ret = -ETIME;
		goto err;
	}

	return 0;
err:
	xe_bo_put(vram);
	return ret;
}

/**
 * xe_gt_sriov_pf_migration_vram_save() - Save VF VRAM migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_vram_save(struct xe_gt *gt, unsigned int vfid)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return pf_save_vf_vram_mig_data(gt, vfid);
}

/**
 * xe_gt_sriov_pf_migration_vram_restore() - Restore VF VRAM migration data.
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_vram_restore(struct xe_gt *gt, unsigned int vfid,
					  struct xe_sriov_pf_migration_data *data)
{
	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));
	xe_gt_assert(gt, vfid != PFID);
	xe_gt_assert(gt, vfid <= xe_sriov_pf_get_totalvfs(gt_to_xe(gt)));

	return pf_restore_vf_vram_mig_data(gt, vfid, data);
}

/**
 * xe_gt_sriov_pf_migration_size() - Total size of migration data from all components within a GT
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * This function is for PF only.
 *
 * Return: total migration data size in bytes or a negative error code on failure.
 */
ssize_t xe_gt_sriov_pf_migration_size(struct xe_gt *gt, unsigned int vfid)
{
	ssize_t total = 0;
	ssize_t size;

	xe_gt_assert(gt, IS_SRIOV_PF(gt_to_xe(gt)));

	size = xe_gt_sriov_pf_migration_guc_size(gt, vfid);
	if (size < 0)
		return size;
	else if (size > 0)
		size += sizeof(struct xe_sriov_pf_migration_hdr);
	total += size;

	size = xe_gt_sriov_pf_migration_ggtt_size(gt, vfid);
	if (size < 0)
		return size;
	else if (size > 0)
		size += sizeof(struct xe_sriov_pf_migration_hdr);
	total += size;

	size = xe_gt_sriov_pf_migration_mmio_size(gt, vfid);
	if (size < 0)
		return size;
	else if (size > 0)
		size += sizeof(struct xe_sriov_pf_migration_hdr);
	total += size;

	size = xe_gt_sriov_pf_migration_vram_size(gt, vfid);
	if (size < 0)
		return size;
	else if (size > 0)
		size += sizeof(struct xe_sriov_pf_migration_hdr);
	total += size;

	return total;
}

/**
 * xe_gt_sriov_pf_migration_ring_empty() - Check if a migration ring is empty
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * Return: true if the ring is empty, otherwise false.
 */
bool xe_gt_sriov_pf_migration_ring_empty(struct xe_gt *gt, unsigned int vfid)
{
	return ptr_ring_empty(&pf_pick_gt_migration(gt, vfid)->ring);
}

/**
 * xe_gt_sriov_pf_migration_produce() - Add migration data packet to migration ring
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 * @data: &struct xe_sriov_pf_migration_data packet
 *
 * If the ring is full, wait until there is space in the ring.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_ring_produce(struct xe_gt *gt, unsigned int vfid,
					  struct xe_sriov_pf_migration_data *data)
{
	struct xe_gt_sriov_pf_migration *migration = pf_pick_gt_migration(gt, vfid);
	struct wait_queue_head *wq = xe_sriov_pf_migration_waitqueue(gt_to_xe(gt), vfid);
	unsigned long timeout = XE_GT_SRIOV_PF_MIGRATION_RING_TIMEOUT;
	int ret;

	xe_gt_assert(gt, data->tile == gt->tile->id);
	xe_gt_assert(gt, data->gt == gt->info.id);

	while (1) {
		ret = ptr_ring_produce(&migration->ring, data);
		if (ret == 0) {
			wake_up_all(wq);
			break;
		}

		if (!xe_gt_sriov_pf_control_check_vf_data_wip(gt, vfid))
			return -EINVAL;

		ret = wait_event_interruptible_timeout(*wq,
						       !ptr_ring_full(&migration->ring),
						       timeout);
		if (ret == 0)
			return -ETIMEDOUT;

		timeout = ret;
	}

	return ret;
}

/**
 * xe_gt_sriov_pf_migration_consume() - Get migration data packet from migration ring
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * If the ring is empty, wait until there are new migration data packets to process.
 *
 * Return: Pointer to &struct xe_sriov_pf_migration_data on success,
 *	   ERR_PTR(-ENODATA) if ring is empty and no more migration data is expected,
 *	   ERR_PTR value in case of error.
 */
struct xe_sriov_pf_migration_data *
xe_gt_sriov_pf_migration_ring_consume(struct xe_gt *gt, unsigned int vfid)
{
	struct xe_gt_sriov_pf_migration *migration = pf_pick_gt_migration(gt, vfid);
	struct wait_queue_head *wq = xe_sriov_pf_migration_waitqueue(gt_to_xe(gt), vfid);
	unsigned long timeout = XE_GT_SRIOV_PF_MIGRATION_RING_TIMEOUT;
	struct xe_sriov_pf_migration_data *data;
	int ret;

	while (1) {
		data = ptr_ring_consume(&migration->ring);
		if (data) {
			wake_up_all(wq);
			break;
		}

		if (!xe_gt_sriov_pf_control_check_vf_data_wip(gt, vfid))
			return ERR_PTR(-ENODATA);

		ret = wait_event_interruptible_timeout(*wq,
					 !ptr_ring_empty(&migration->ring) ||
					 !xe_gt_sriov_pf_control_check_vf_data_wip(gt, vfid),
					 timeout);
		if (ret == 0)
			return ERR_PTR(-ETIMEDOUT);

		timeout = ret;
	}

	return data;
}

/**
 * xe_gt_sriov_pf_migration_consume_nowait() - Get migration data packet from migration ring
 * @gt: the &struct xe_gt
 * @vfid: the VF identifier
 *
 * Similar to xe_gt_sriov_pf_migration_consume(), but doesn't wait until more data is available.
 *
 * Return: Pointer to &struct xe_sriov_pf_migration_data on success,
 *	   ERR_PTR(-EAGAIN) if ring is empty but migration data is expected,
 *	   ERR_PTR(-ENODATA) if ring is empty and no more migration data is expected,
 *	   ERR_PTR value in case of error.
 */
struct xe_sriov_pf_migration_data *
xe_gt_sriov_pf_migration_ring_consume_nowait(struct xe_gt *gt, unsigned int vfid)
{
	struct xe_gt_sriov_pf_migration *migration = pf_pick_gt_migration(gt, vfid);
	struct wait_queue_head *wq = xe_sriov_pf_migration_waitqueue(gt_to_xe(gt), vfid);
	struct xe_sriov_pf_migration_data *data;

	data = ptr_ring_consume(&migration->ring);
	if (data) {
		wake_up_all(wq);
		return data;
	}

	if (!xe_gt_sriov_pf_control_check_vf_data_wip(gt, vfid))
		return ERR_PTR(-ENODATA);

	return ERR_PTR(-EAGAIN);
}

static void pf_mig_data_destroy(void *ptr)
{
	struct xe_sriov_pf_migration_data *data = ptr;

	xe_sriov_pf_migration_data_free(data);
}

static void pf_gt_migration_cleanup(struct drm_device *dev, void *arg)
{
	struct xe_gt_sriov_pf_migration *migration = arg;

	ptr_ring_cleanup(&migration->ring, pf_mig_data_destroy);
}

/**
 * xe_gt_sriov_pf_migration_init() - Initialize support for VF migration.
 * @gt: the &xe_gt
 *
 * This function is for PF only.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int xe_gt_sriov_pf_migration_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	unsigned int n, totalvfs;
	int err;

	xe_gt_assert(gt, IS_SRIOV_PF(xe));

	if (!pf_migration_supported(gt))
		return 0;

	err = drmm_mutex_init(&xe->drm, &gt->sriov.pf.snapshot_lock);
	if (err)
		return err;

	totalvfs = xe_sriov_pf_get_totalvfs(xe);
	for (n = 0; n <= totalvfs; n++) {
		struct xe_gt_sriov_pf_migration *migration = pf_pick_gt_migration(gt, n);

		err = ptr_ring_init(&migration->ring,
				    XE_GT_SRIOV_PF_MIGRATION_RING_SIZE, GFP_KERNEL);
		if (err)
			return err;

		err = drmm_add_action_or_reset(&xe->drm, pf_gt_migration_cleanup, migration);
		if (err)
			return err;
	}

	return 0;
}
