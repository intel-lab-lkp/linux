// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#include <drm/drm_cache.h>
#include <linux/cred.h>
#include <linux/errno.h>

#include "aie.h"
#include "amdxdna_ctx.h"
#include "amdxdna_mailbox_helper.h"
#include "amdxdna_mailbox.h"
#include "amdxdna_pci_drv.h"

void aie_dump_mgmt_chann_debug(struct aie_device *aie)
{
	struct amdxdna_dev *xdna = aie->xdna;

	XDNA_DBG(xdna, "i2x tail    0x%x", aie->mgmt_i2x.mb_tail_ptr_reg);
	XDNA_DBG(xdna, "i2x head    0x%x", aie->mgmt_i2x.mb_head_ptr_reg);
	XDNA_DBG(xdna, "i2x ringbuf 0x%x", aie->mgmt_i2x.rb_start_addr);
	XDNA_DBG(xdna, "i2x rsize   0x%x", aie->mgmt_i2x.rb_size);
	XDNA_DBG(xdna, "x2i tail    0x%x", aie->mgmt_x2i.mb_tail_ptr_reg);
	XDNA_DBG(xdna, "x2i head    0x%x", aie->mgmt_x2i.mb_head_ptr_reg);
	XDNA_DBG(xdna, "x2i ringbuf 0x%x", aie->mgmt_x2i.rb_start_addr);
	XDNA_DBG(xdna, "x2i rsize   0x%x", aie->mgmt_x2i.rb_size);
	XDNA_DBG(xdna, "x2i chann index 0x%x", aie->mgmt_chan_idx);
	XDNA_DBG(xdna, "mailbox protocol major 0x%x", aie->mgmt_prot_major);
	XDNA_DBG(xdna, "mailbox protocol minor 0x%x", aie->mgmt_prot_minor);
}

void aie_destroy_chann(struct aie_device *aie, struct mailbox_channel **chann)
{
	struct amdxdna_dev *xdna = aie->xdna;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (!*chann)
		return;

	xdna_mailbox_stop_channel(*chann);
	xdna_mailbox_free_channel(*chann);
	*chann = NULL;
}

int aie_send_mgmt_msg_wait(struct aie_device *aie, struct xdna_mailbox_msg *msg)
{
	struct amdxdna_dev *xdna = aie->xdna;
	struct xdna_notify *hdl = msg->handle;
	int ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (!aie->mgmt_chann)
		return -ENODEV;

	ret = xdna_send_msg_wait(xdna, aie->mgmt_chann, msg);
	if (ret == -ETIME)
		aie_destroy_chann(aie, &aie->mgmt_chann);

	if (!ret && *hdl->status) {
		XDNA_ERR(xdna, "command opcode 0x%x failed, status 0x%x",
			 msg->opcode, *hdl->data);
		ret = -EINVAL;
	}

	return ret;
}

int aie_check_protocol(struct aie_device *aie, u32 fw_major, u32 fw_minor)
{
	const struct amdxdna_fw_feature_tbl *feature;
	bool found = false;

	for (feature = aie->xdna->dev_info->fw_feature_tbl;
	     feature->major; feature++) {
		if (feature->major != fw_major)
			continue;
		if (fw_minor < feature->min_minor)
			continue;
		if (feature->max_minor > 0 && fw_minor > feature->max_minor)
			continue;

		aie->feature_mask |= feature->features;

		/* firmware version matches one of the driver support entry */
		found = true;
	}

	return found ? 0 : -EOPNOTSUPP;
}

static void amdxdna_update_vbnv(struct amdxdna_dev *xdna,
				const struct amdxdna_rev_vbnv *tbl,
				u32 rev)
{
	int i;

	for (i = 0; tbl[i].vbnv; i++) {
		if (tbl[i].revision == rev) {
			xdna->vbnv = tbl[i].vbnv;
			break;
		}
	}
}

void amdxdna_vbnv_init(struct amdxdna_dev *xdna)
{
	const struct amdxdna_dev_info *info = xdna->dev_info;
	u32 rev;

	xdna->vbnv = info->default_vbnv;

	if (!info->ops->get_dev_revision || !info->rev_vbnv_tbl)
		return;

	if (info->ops->get_dev_revision(xdna, &rev))
		return;

	amdxdna_update_vbnv(xdna, info->rev_vbnv_tbl, rev);
}

int amdxdna_get_metadata(struct aie_device *aie,
			 struct amdxdna_client *client,
			 struct amdxdna_drm_get_info *args)
{
	int ret = 0;
	u32 buf_sz;

	buf_sz = min(args->buffer_size, sizeof(aie->metadata));
	if (copy_to_user(u64_to_user_ptr(args->buffer), &aie->metadata, buf_sz))
		ret = -EFAULT;

	return ret;
}

struct amdxdna_msg_buf_hdl *amdxdna_alloc_msg_buffer(struct amdxdna_dev *xdna, u32 size)
{
	struct amdxdna_msg_buf_hdl *hdl;
	int order;

	hdl = kzalloc_obj(*hdl);
	if (!hdl)
		return ERR_PTR(-ENOMEM);

	hdl->xdna = xdna;
	hdl->size = max_t(u32, size, SZ_8K);
	order = get_order(hdl->size);
	if (order > MAX_PAGE_ORDER)
		goto free_hdl;
	hdl->size = PAGE_SIZE << order;

	if (amdxdna_iova_on(xdna)) {
		hdl->vaddr = amdxdna_iommu_alloc(xdna, hdl->size, &hdl->dma_addr);
		if (IS_ERR(hdl->vaddr))
			goto free_hdl;
	} else {
		hdl->vaddr = dma_alloc_noncoherent(xdna->ddev.dev, hdl->size,
						   &hdl->dma_addr,
						   DMA_FROM_DEVICE, GFP_KERNEL);
		if (!hdl->vaddr)
			goto free_hdl;
	}

	return hdl;

free_hdl:
	kfree(hdl);
	return ERR_PTR(-ENOMEM);
}

void amdxdna_free_msg_buffer(struct amdxdna_msg_buf_hdl *hdl)
{
	if (!hdl)
		return;

	if (amdxdna_iova_on(hdl->xdna)) {
		amdxdna_iommu_free(hdl->xdna, hdl->size, hdl->vaddr,
				   hdl->dma_addr);
	} else {
		dma_free_noncoherent(hdl->xdna->ddev.dev, hdl->size,
				     hdl->vaddr, hdl->dma_addr,
				     DMA_FROM_DEVICE);
	}

	kfree(hdl);
}

struct amdxdna_coredump_walk_arg {
	u64				pid;
	u32				ctx_id;

	struct aie_device		*aie;
	struct amdxdna_drm_get_array	*array_args;
};

static int amdxdna_get_coredump_cb(struct amdxdna_hwctx *hwctx, void *arg)
{
	struct amdxdna_dev *xdna = hwctx->client->xdna;
	struct amdxdna_coredump_buf_entry *buf_list;
	struct amdxdna_coredump_walk_arg *wa = arg;
	struct amdxdna_msg_buf_hdl **data_hdls;
	struct amdxdna_msg_buf_hdl *list_hdl;
	struct aie_device *aie = wa->aie;
	size_t data_buf_size = SZ_1M;
	size_t total_size;
	u8 __user *buf;
	u32 num_bufs;
	int ret, i;

	if (hwctx->client->pid != wa->pid || hwctx->id != wa->ctx_id)
		return 0;

	if (!capable(CAP_SYS_ADMIN) &&
	    !uid_eq(current_euid(), hwctx->client->filp->filp->f_cred->euid)) {
		XDNA_ERR(xdna, "Permission denied for context %u", wa->ctx_id);
		return -EPERM;
	}

	num_bufs = (hwctx->num_col - hwctx->num_unused_col) * aie->metadata.rows;
	total_size = data_buf_size * num_bufs;

	if (wa->array_args->element_size < total_size) {
		XDNA_DBG(xdna, "Insufficient buffer size %u, need %zu",
			 wa->array_args->element_size, total_size);
		wa->array_args->element_size = total_size;
		return -ENOSPC;
	}

	list_hdl = amdxdna_alloc_msg_buffer(xdna, num_bufs * sizeof(*buf_list));
	if (IS_ERR(list_hdl)) {
		XDNA_ERR(xdna, "Failed to allocate buffer list");
		return PTR_ERR(list_hdl);
	}

	buf_list = to_cpu_addr(list_hdl, 0);
	memset(buf_list, 0, to_buf_size(list_hdl));

	data_hdls = kzalloc_objs(*data_hdls, num_bufs);
	if (!data_hdls) {
		ret = -ENOMEM;
		goto free_list_hdl;
	}

	for (i = 0; i < num_bufs; i++) {
		data_hdls[i] = amdxdna_alloc_msg_buffer(xdna, data_buf_size);
		if (IS_ERR(data_hdls[i])) {
			XDNA_ERR(xdna, "Failed to allocate data buffer %d", i);
			ret = PTR_ERR(data_hdls[i]);
			data_hdls[i] = NULL;
			goto free_data_hdls;
		}

		memset(to_cpu_addr(data_hdls[i], 0), 0, data_buf_size);
		drm_clflush_virt_range(to_cpu_addr(data_hdls[i], 0), data_buf_size);

		buf_list[i].buf_addr = to_dma_addr(data_hdls[i], 0);
		buf_list[i].buf_size = data_buf_size;
	}

	drm_clflush_virt_range(buf_list, to_buf_size(list_hdl));

	ret = aie->msg_ops.get_coredump(hwctx, list_hdl, num_bufs);
	if (ret) {
		XDNA_ERR(xdna, "Failed to get coredump from firmware, ret=%d",
			 ret);
		goto free_data_hdls;
	}

	buf = u64_to_user_ptr(wa->array_args->buffer);
	for (i = 0; i < num_bufs; i++) {
		if (copy_to_user(buf, to_cpu_addr(data_hdls[i], 0),
				 data_buf_size)) {
			ret = -EFAULT;
			goto free_data_hdls;
		}
		buf += data_buf_size;
	}

free_data_hdls:
	for (i = 0; i < num_bufs; i++)
		amdxdna_free_msg_buffer(data_hdls[i]);
	kfree(data_hdls);
free_list_hdl:
	amdxdna_free_msg_buffer(list_hdl);
	return ret ? ret : 1;
}

int amdxdna_get_coredump(struct aie_device *aie,
			 struct amdxdna_drm_get_array *args)
{
	struct amdxdna_client *ctx_client = NULL;
	struct amdxdna_drm_aie_coredump config;
	struct amdxdna_dev *xdna = aie->xdna;
	struct amdxdna_coredump_walk_arg wa;
	int ret;

	drm_WARN_ON(&xdna->ddev, !mutex_is_locked(&xdna->dev_lock));

	if (!aie->msg_ops.get_coredump) {
		XDNA_DBG(xdna, "Get coredump unsupported");
		return -EOPNOTSUPP;
	}
	if (args->num_element != 1) {
		XDNA_ERR(xdna, "Invalid num_element %u, expected 1",
			 args->num_element);
		return -EINVAL;
	}

	if (args->element_size < sizeof(config)) {
		XDNA_ERR(xdna, "Invalid buffer size: %d", args->element_size);
		return -EINVAL;
	}

	if (copy_from_user(&config, u64_to_user_ptr(args->buffer), sizeof(config))) {
		XDNA_ERR(xdna, "Failed to copy coredump config from user");
		return -EFAULT;
	}

	if (XDNA_MBZ_DBG(xdna, &config.pad, sizeof(config.pad)))
		return -EINVAL;

	XDNA_DBG(xdna, "AIE Coredump request for context_id=%u pid=%llu",
		 config.context_id, config.pid);

	wa.aie = aie;
	wa.pid = config.pid;
	wa.ctx_id = config.context_id;
	wa.array_args = args;
	amdxdna_for_each_client(xdna, ctx_client) {
		ret = amdxdna_hwctx_walk(ctx_client, &wa, amdxdna_get_coredump_cb);
		if (ret)
			break;
	}

	if (!ret) {
		XDNA_DBG(xdna, "Context not found");
		return -EINVAL;
	}

	return (ret > 0) ? 0 : ret;
}
