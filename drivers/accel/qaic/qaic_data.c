// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2019-2021, The Linux Foundation. All rights reserved. */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/srcu.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_print.h>
#include <uapi/drm/qaic_accel.h>

#include "qaic.h"

#define SEM_VAL_MASK	GENMASK_ULL(11, 0)
#define SEM_INDEX_MASK	GENMASK_ULL(4, 0)
#define BULK_XFER	BIT(3)
#define GEN_COMPLETION	BIT(4)
#define INBOUND_XFER	1
#define OUTBOUND_XFER	2
#define REQHP_OFF	0x0 /* we read this */
#define REQTP_OFF	0x4 /* we write this */
#define RSPHP_OFF	0x8 /* we write this */
#define RSPTP_OFF	0xc /* we read this */

#define ENCODE_SEM(val, index, sync, cmd, flags)			\
		({							\
			FIELD_PREP(GENMASK(11, 0), (val)) |		\
			FIELD_PREP(GENMASK(20, 16), (index)) |		\
			FIELD_PREP(BIT(22), (sync)) |			\
			FIELD_PREP(GENMASK(26, 24), (cmd)) |		\
			FIELD_PREP(GENMASK(30, 29), (flags)) |		\
			FIELD_PREP(BIT(31), (cmd) ? 1 : 0);		\
		})
#define NUM_EVENTS	128
#define NUM_DELAYS	10
#define fifo_at(base, offset) ((base) + (offset) * get_dbc_req_elem_size())

static unsigned int wait_exec_default_timeout_ms = 5000; /* 5 sec default */
module_param(wait_exec_default_timeout_ms, uint, 0600);
MODULE_PARM_DESC(wait_exec_default_timeout_ms, "Default timeout for DRM_IOCTL_QAIC_WAIT_BO");

static unsigned int datapath_poll_interval_us = 100; /* 100 usec default */
module_param(datapath_poll_interval_us, uint, 0600);
MODULE_PARM_DESC(datapath_poll_interval_us,
		 "Amount of time to sleep between activity when datapath polling is enabled");

struct dbc_rsp {
	/* Request ID of the memory handle whose DMA transaction is completed */
	__le16	req_id;
	/* Status of the DMA transaction. 0 : Success otherwise failure */
	__le16	status;
} __packed;

/* Common data structure for all the variants of execute IOCTL */
struct execute_info {
	/* Number of entries present in arrays */
	u32 count;
	struct {
		u64 received_ts;
		u64 submit_ts;
		u32 queue_level;
	} perf_stats;
	/* Array of pointers to BOs for this execution */
	struct qaic_bo **bo_arr;
	/* Array of handles to above BOs, helpful for identifying failures */
	u32 *handle_arr;
	/* Array of new sizes for BO transfer if request is_partial */
	u64 *resize_arr;
	/* True if execution is of resized BO */
	bool is_partial;
};

/* Helper structure for retrieving info about execution entries */
struct qaic_exec_data_ent {
	/* Pointer to requested gem object */
	struct drm_gem_object *obj;
	/* New size for BO transfer if request is partial execute */
	u64 resize;
	/* Per-file handle for BO, helpful for identifying failures */
	u32 handle;
};

/* Helper structure for requesting info about execution entries */
struct qaic_exec_data_req {
	/* Pointer to drm file */
	struct drm_file *drm_file;
	/* Pointer to beginning of array of execution entries from user */
	void *exec_data;
	/* Is exec_data an array of partial execution entries? */
	bool is_partial;
	/* Index of exec_data array this request is acting upon */
	int idx;
};

/* Caller should be holding bo->lock */
static inline bool bo_queued(struct qaic_bo *bo)
{
	if (bo->fence)
		return !dma_fence_is_signaled(bo->fence);
	return false;
}

inline int get_dbc_req_elem_size(void)
{
	return sizeof(struct dbc_req);
}

inline int get_dbc_rsp_elem_size(void)
{
	return sizeof(struct dbc_rsp);
}

static void free_slice(struct kref *kref)
{
	struct bo_slice *slice = container_of(kref, struct bo_slice, ref_count);

	slice->bo->total_slice_nents -= slice->nents;
	list_del(&slice->slice);
	drm_gem_object_put(&slice->bo->base);
	sg_free_table(slice->sgt);
	kfree(slice->sgt);
	kvfree(slice->reqs);
	kfree(slice);
}

static int clone_range_of_sgt_for_slice(struct qaic_device *qdev, struct sg_table **sgt_out,
					struct sg_table *sgt_in, u64 size, u64 offset)
{
	struct scatterlist *sg, *sgn, *sgf, *sgl;
	unsigned int len, nents, offf, offl;
	struct sg_table *sgt;
	size_t total_len;
	int ret, j;

	/* find out number of relevant nents needed for this mem */
	total_len = 0;
	sgf = NULL;
	sgl = NULL;
	nents = 0;
	offf = 0;
	offl = 0;

	size = size ? size : PAGE_SIZE;
	for_each_sgtable_dma_sg(sgt_in, sg, j) {
		len = sg_dma_len(sg);

		if (!len)
			continue;
		if (offset >= total_len && offset < total_len + len) {
			sgf = sg;
			offf = offset - total_len;
		}
		if (sgf)
			nents++;
		if (offset + size >= total_len &&
		    offset + size <= total_len + len) {
			sgl = sg;
			offl = offset + size - total_len;
			break;
		}
		total_len += len;
	}

	if (!sgf || !sgl) {
		ret = -EINVAL;
		goto out;
	}

	sgt = kzalloc_obj(*sgt);
	if (!sgt) {
		ret = -ENOMEM;
		goto out;
	}

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret)
		goto free_sgt;

	/* copy relevant sg node and fix page and length */
	sgn = sgf;
	for_each_sgtable_dma_sg(sgt, sg, j) {
		memcpy(sg, sgn, sizeof(*sg));
		if (sgn == sgf) {
			sg_dma_address(sg) += offf;
			sg_dma_len(sg) -= offf;
			sg_set_page(sg, sg_page(sgn), sg_dma_len(sg), offf);
		} else {
			offf = 0;
		}
		if (sgn == sgl) {
			sg_dma_len(sg) = offl - offf;
			sg_set_page(sg, sg_page(sgn), offl - offf, offf);
			sg_mark_end(sg);
			break;
		}
		sgn = sg_next(sgn);
	}

	*sgt_out = sgt;
	return ret;

free_sgt:
	kfree(sgt);
out:
	*sgt_out = NULL;
	return ret;
}

static int encode_reqs(struct qaic_device *qdev, struct bo_slice *slice,
		       struct qaic_attach_slice_entry *req)
{
	__le64 db_addr = cpu_to_le64(req->db_addr);
	__le32 db_data = cpu_to_le32(req->db_data);
	struct scatterlist *sg;
	__u8 cmd = BULK_XFER;
	int presync_sem;
	u64 dev_addr;
	__u8 db_len;
	int i;

	if (!slice->no_xfer)
		cmd |= (slice->dir == DMA_TO_DEVICE ? INBOUND_XFER : OUTBOUND_XFER);

	if (req->db_len && !IS_ALIGNED(req->db_addr, req->db_len / 8))
		return -EINVAL;

	presync_sem = req->sem0.presync + req->sem1.presync + req->sem2.presync + req->sem3.presync;
	if (presync_sem > 1)
		return -EINVAL;

	presync_sem = req->sem0.presync << 0 | req->sem1.presync << 1 |
		      req->sem2.presync << 2 | req->sem3.presync << 3;

	switch (req->db_len) {
	case 32:
		db_len = BIT(7);
		break;
	case 16:
		db_len = BIT(7) | 1;
		break;
	case 8:
		db_len = BIT(7) | 2;
		break;
	case 0:
		db_len = 0; /* doorbell is not active for this command */
		break;
	default:
		return -EINVAL; /* should never hit this */
	}

	/*
	 * When we end up splitting up a single request (ie a buf slice) into
	 * multiple DMA requests, we have to manage the sync data carefully.
	 * There can only be one presync sem. That needs to be on every xfer
	 * so that the DMA engine doesn't transfer data before the receiver is
	 * ready. We only do the doorbell and postsync sems after the xfer.
	 * To guarantee previous xfers for the request are complete, we use a
	 * fence.
	 */
	dev_addr = req->dev_addr;
	for_each_sgtable_dma_sg(slice->sgt, sg, i) {
		slice->reqs[i].cmd = cmd;
		slice->reqs[i].src_addr = cpu_to_le64(slice->dir == DMA_TO_DEVICE ?
						      sg_dma_address(sg) : dev_addr);
		slice->reqs[i].dest_addr = cpu_to_le64(slice->dir == DMA_TO_DEVICE ?
						       dev_addr : sg_dma_address(sg));
		/*
		 * sg_dma_len(sg) returns size of a DMA segment, maximum DMA
		 * segment size is set to UINT_MAX by qaic and hence return
		 * values of sg_dma_len(sg) can never exceed u32 range. So,
		 * by down sizing we are not corrupting the value.
		 */
		slice->reqs[i].len = cpu_to_le32((u32)sg_dma_len(sg));
		switch (presync_sem) {
		case BIT(0):
			slice->reqs[i].sem_cmd0 = cpu_to_le32(ENCODE_SEM(req->sem0.val,
									 req->sem0.index,
									 req->sem0.presync,
									 req->sem0.cmd,
									 req->sem0.flags));
			break;
		case BIT(1):
			slice->reqs[i].sem_cmd1 = cpu_to_le32(ENCODE_SEM(req->sem1.val,
									 req->sem1.index,
									 req->sem1.presync,
									 req->sem1.cmd,
									 req->sem1.flags));
			break;
		case BIT(2):
			slice->reqs[i].sem_cmd2 = cpu_to_le32(ENCODE_SEM(req->sem2.val,
									 req->sem2.index,
									 req->sem2.presync,
									 req->sem2.cmd,
									 req->sem2.flags));
			break;
		case BIT(3):
			slice->reqs[i].sem_cmd3 = cpu_to_le32(ENCODE_SEM(req->sem3.val,
									 req->sem3.index,
									 req->sem3.presync,
									 req->sem3.cmd,
									 req->sem3.flags));
			break;
		}
		dev_addr += sg_dma_len(sg);
	}
	/* add post transfer stuff to last segment */
	i--;
	slice->reqs[i].cmd |= GEN_COMPLETION;
	slice->reqs[i].db_addr = db_addr;
	slice->reqs[i].db_len = db_len;
	slice->reqs[i].db_data = db_data;
	/*
	 * Add a fence if we have more than one request going to the hardware
	 * representing the entirety of the user request, and the user request
	 * has no presync condition.
	 * Fences are expensive, so we try to avoid them. We rely on the
	 * hardware behavior to avoid needing one when there is a presync
	 * condition. When a presync exists, all requests for that same
	 * presync will be queued into a fifo. Thus, since we queue the
	 * post xfer activity only on the last request we queue, the hardware
	 * will ensure that the last queued request is processed last, thus
	 * making sure the post xfer activity happens at the right time without
	 * a fence.
	 */
	if (i && !presync_sem)
		req->sem0.flags |= (slice->dir == DMA_TO_DEVICE ?
				    QAIC_SEM_INSYNCFENCE : QAIC_SEM_OUTSYNCFENCE);
	slice->reqs[i].sem_cmd0 = cpu_to_le32(ENCODE_SEM(req->sem0.val, req->sem0.index,
							 req->sem0.presync, req->sem0.cmd,
							 req->sem0.flags));
	slice->reqs[i].sem_cmd1 = cpu_to_le32(ENCODE_SEM(req->sem1.val, req->sem1.index,
							 req->sem1.presync, req->sem1.cmd,
							 req->sem1.flags));
	slice->reqs[i].sem_cmd2 = cpu_to_le32(ENCODE_SEM(req->sem2.val, req->sem2.index,
							 req->sem2.presync, req->sem2.cmd,
							 req->sem2.flags));
	slice->reqs[i].sem_cmd3 = cpu_to_le32(ENCODE_SEM(req->sem3.val, req->sem3.index,
							 req->sem3.presync, req->sem3.cmd,
							 req->sem3.flags));

	return 0;
}

static int qaic_map_one_slice(struct qaic_device *qdev, struct qaic_bo *bo,
			      struct qaic_attach_slice_entry *slice_ent)
{
	struct sg_table *sgt = NULL;
	struct bo_slice *slice;
	int ret;

	ret = clone_range_of_sgt_for_slice(qdev, &sgt, bo->sgt, slice_ent->size, slice_ent->offset);
	if (ret)
		goto out;

	slice = kmalloc_obj(*slice);
	if (!slice) {
		ret = -ENOMEM;
		goto free_sgt;
	}

	slice->reqs = kvzalloc_objs(*slice->reqs, sgt->nents);
	if (!slice->reqs) {
		ret = -ENOMEM;
		goto free_slice;
	}

	slice->no_xfer = !slice_ent->size;
	slice->sgt = sgt;
	slice->nents = sgt->nents;
	slice->dir = bo->dir;
	slice->bo = bo;
	slice->size = slice_ent->size;
	slice->offset = slice_ent->offset;

	ret = encode_reqs(qdev, slice, slice_ent);
	if (ret)
		goto free_req;

	bo->total_slice_nents += sgt->nents;
	kref_init(&slice->ref_count);
	drm_gem_object_get(&bo->base);
	list_add_tail(&slice->slice, &bo->slices);

	return 0;

free_req:
	kvfree(slice->reqs);
free_slice:
	kfree(slice);
free_sgt:
	sg_free_table(sgt);
	kfree(sgt);
out:
	return ret;
}

static int create_sgt(struct qaic_device *qdev, struct sg_table **sgt_out, u64 size)
{
	struct scatterlist *sg;
	struct sg_table *sgt;
	struct page **pages;
	int *pages_order;
	int buf_extra;
	int max_order;
	int nr_pages;
	int ret = 0;
	int i, j, k;
	int order;

	if (size) {
		u64 nr_pages_u64 = DIV_ROUND_UP_ULL(size, PAGE_SIZE);

		if (overflows_type(nr_pages_u64, int))
			return -E2BIG;

		nr_pages = nr_pages_u64;
		/*
		 * calculate how much extra we are going to allocate, to remove
		 * later
		 */
		buf_extra = (PAGE_SIZE - size % PAGE_SIZE) % PAGE_SIZE;
		max_order = min(MAX_PAGE_ORDER, get_order(size));
	} else {
		/* allocate a single page for book keeping */
		nr_pages = 1;
		buf_extra = 0;
		max_order = 0;
	}

	pages = kvmalloc_array(nr_pages, sizeof(*pages) + sizeof(*pages_order), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto out;
	}
	pages_order = (void *)pages + sizeof(*pages) * nr_pages;

	/*
	 * Allocate requested memory using alloc_pages. It is possible to allocate
	 * the requested memory in multiple chunks by calling alloc_pages
	 * multiple times. Use SG table to handle multiple allocated pages.
	 */
	i = 0;
	while (nr_pages > 0) {
		order = min(get_order(nr_pages * PAGE_SIZE), max_order);
		while (1) {
			pages[i] = alloc_pages(GFP_KERNEL | GFP_HIGHUSER |
					       __GFP_NOWARN | __GFP_ZERO |
					       (order ? __GFP_NORETRY : __GFP_RETRY_MAYFAIL),
					       order);
			if (pages[i])
				break;
			if (!order--) {
				ret = -ENOMEM;
				goto free_partial_alloc;
			}
		}

		max_order = order;
		pages_order[i] = order;

		nr_pages -= 1 << order;
		if (nr_pages <= 0)
			/* account for over allocation */
			buf_extra += abs(nr_pages) * PAGE_SIZE;
		i++;
	}

	sgt = kmalloc_obj(*sgt);
	if (!sgt) {
		ret = -ENOMEM;
		goto free_partial_alloc;
	}

	if (sg_alloc_table(sgt, i, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto free_sgt;
	}

	/* Populate the SG table with the allocated memory pages */
	sg = sgt->sgl;
	for (k = 0; k < i; k++, sg = sg_next(sg)) {
		/* Last entry requires special handling */
		if (k < i - 1) {
			sg_set_page(sg, pages[k], PAGE_SIZE << pages_order[k], 0);
		} else {
			sg_set_page(sg, pages[k], (PAGE_SIZE << pages_order[k]) - buf_extra, 0);
			sg_mark_end(sg);
		}
	}

	kvfree(pages);
	*sgt_out = sgt;
	return ret;

free_sgt:
	kfree(sgt);
free_partial_alloc:
	for (j = 0; j < i; j++)
		__free_pages(pages[j], pages_order[j]);
	kvfree(pages);
out:
	*sgt_out = NULL;
	return ret;
}

static bool invalid_sem(struct qaic_sem *sem)
{
	if (sem->val & ~SEM_VAL_MASK || sem->index & ~SEM_INDEX_MASK ||
	    !(sem->presync == 0 || sem->presync == 1) || sem->pad ||
	    sem->flags & ~(QAIC_SEM_INSYNCFENCE | QAIC_SEM_OUTSYNCFENCE) ||
	    sem->cmd > QAIC_SEM_WAIT_GT_0)
		return true;
	return false;
}

static int qaic_validate_req(struct qaic_device *qdev, struct qaic_attach_slice_entry *slice_ent,
			     u32 count, u64 total_size)
{
	u64 total;
	int i;

	for (i = 0; i < count; i++) {
		if (!(slice_ent[i].db_len == 32 || slice_ent[i].db_len == 16 ||
		      slice_ent[i].db_len == 8 || slice_ent[i].db_len == 0) ||
		      invalid_sem(&slice_ent[i].sem0) || invalid_sem(&slice_ent[i].sem1) ||
		      invalid_sem(&slice_ent[i].sem2) || invalid_sem(&slice_ent[i].sem3))
			return -EINVAL;

		if (check_add_overflow(slice_ent[i].offset, slice_ent[i].size, &total) ||
		    total > total_size)
			return -EINVAL;
	}

	return 0;
}

static void qaic_free_sgt(struct sg_table *sgt)
{
	struct scatterlist *sg;

	if (!sgt)
		return;

	for (sg = sgt->sgl; sg; sg = sg_next(sg))
		if (sg_page(sg))
			__free_pages(sg_page(sg), get_order(sg->length));
	sg_free_table(sgt);
	kfree(sgt);
}

static void qaic_gem_print_info(struct drm_printer *p, unsigned int indent,
				const struct drm_gem_object *obj)
{
	struct qaic_bo *bo = to_qaic_bo(obj);

	drm_printf_indent(p, indent, "BO DMA direction %d\n", bo->dir);
}

static const struct vm_operations_struct drm_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static int qaic_gem_object_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct qaic_bo *bo = to_qaic_bo(obj);
	unsigned long remap_start;
	unsigned long offset = 0;
	unsigned long remap_end;
	struct scatterlist *sg;
	unsigned long length;
	int ret = 0;

	if (drm_gem_is_imported(obj))
		return -EINVAL;

	for (sg = bo->sgt->sgl; sg; sg = sg_next(sg)) {
		if (sg_page(sg)) {
			/* if sg is too large for the VMA, so truncate it to fit */
			if (check_add_overflow(vma->vm_start, offset, &remap_start))
				return -EINVAL;
			if (check_add_overflow(remap_start, sg->length, &remap_end))
				return -EINVAL;

			if (remap_end > vma->vm_end) {
				if (check_sub_overflow(vma->vm_end, remap_start, &length))
					return -EINVAL;
			} else {
				length = sg->length;
			}

			if (length == 0)
				goto out;

			ret = remap_pfn_range(vma, vma->vm_start + offset, page_to_pfn(sg_page(sg)),
					      length, vma->vm_page_prot);
			if (ret)
				goto out;
			offset += length;
		}
	}

out:
	return ret;
}

static void qaic_free_object(struct drm_gem_object *obj)
{
	struct qaic_bo *bo = to_qaic_bo(obj);

	if (drm_gem_is_imported(obj)) {
		/* DMABUF/PRIME Path */
		drm_prime_gem_destroy(obj, NULL);
	} else {
		/* Private buffer allocation path */
		qaic_free_sgt(bo->sgt);
	}

	mutex_destroy(&bo->lock);
	drm_gem_object_release(obj);
	kfree(bo);
}

static struct sg_table *qaic_get_sg_table(struct drm_gem_object *obj)
{
	struct qaic_bo *bo = to_qaic_bo(obj);
	struct scatterlist *sg, *sg_in;
	struct sg_table *sgt, *sgt_in;
	int i;

	sgt_in = bo->sgt;

	sgt = kmalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (sg_alloc_table(sgt, sgt_in->orig_nents, GFP_KERNEL)) {
		kfree(sgt);
		return ERR_PTR(-ENOMEM);
	}

	sg = sgt->sgl;
	for_each_sgtable_sg(sgt_in, sg_in, i) {
		memcpy(sg, sg_in, sizeof(*sg));
		sg = sg_next(sg);
	}

	return sgt;
}

static const struct drm_gem_object_funcs qaic_gem_funcs = {
	.free = qaic_free_object,
	.get_sg_table = qaic_get_sg_table,
	.print_info = qaic_gem_print_info,
	.mmap = qaic_gem_object_mmap,
	.vm_ops = &drm_vm_ops,
};

static void qaic_init_bo_lists(struct qaic_bo *bo)
{
	INIT_LIST_HEAD(&bo->slices);
}

static struct qaic_bo *qaic_alloc_init_bo(void)
{
	struct qaic_bo *bo;

	bo = kzalloc_obj(*bo);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	mutex_init(&bo->lock);
	bo->fence_context = dma_fence_context_alloc(1);
	qaic_init_bo_lists(bo);

	return bo;
}

int qaic_create_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qaic_create_bo *args = data;
	int usr_rcu_id, qdev_rcu_id;
	struct drm_gem_object *obj;
	struct qaic_device *qdev;
	struct qaic_user *usr;
	struct qaic_bo *bo;
	size_t size;
	int ret;

	if (args->pad)
		return -EINVAL;

	size = PAGE_ALIGN(args->size);
	if (size == 0)
		return -EINVAL;

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	bo = qaic_alloc_init_bo();
	if (IS_ERR(bo)) {
		ret = PTR_ERR(bo);
		goto unlock_dev_srcu;
	}
	obj = &bo->base;

	drm_gem_private_object_init(dev, obj, size);

	obj->funcs = &qaic_gem_funcs;
	ret = create_sgt(qdev, &bo->sgt, size);
	if (ret)
		goto free_bo;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto free_bo;

	ret = drm_gem_handle_create(file_priv, obj, &args->handle);
	if (ret)
		goto free_bo;

	drm_gem_object_put(obj);
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);

	return 0;

free_bo:
	drm_gem_object_put(obj);
unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	return ret;
}

int qaic_mmap_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qaic_mmap_bo *args = data;
	int usr_rcu_id, qdev_rcu_id;
	struct drm_gem_object *obj;
	struct qaic_device *qdev;
	struct qaic_user *usr;
	int ret = 0;

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock_dev_srcu;
	}

	args->offset = drm_vma_node_offset_addr(&obj->vma_node);

	drm_gem_object_put(obj);

unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	return ret;
}

struct drm_gem_object *qaic_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct dma_buf_attachment *attach;
	struct drm_gem_object *obj;
	struct qaic_bo *bo;
	int ret;

	bo = qaic_alloc_init_bo();
	if (IS_ERR(bo)) {
		ret = PTR_ERR(bo);
		goto out;
	}

	obj = &bo->base;
	get_dma_buf(dma_buf);

	attach = dma_buf_attach(dma_buf, dev->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto attach_fail;
	}

	if (!attach->dmabuf->size) {
		ret = -EINVAL;
		goto size_align_fail;
	}

	drm_gem_private_object_init(dev, obj, attach->dmabuf->size);
	/*
	 * skipping dma_buf_map_attachment() as we do not know the direction
	 * just yet. Once the direction is known in the subsequent IOCTL to
	 * attach slicing, we can do it then.
	 */

	obj->funcs = &qaic_gem_funcs;
	obj->import_attach = attach;
	obj->resv = dma_buf->resv;

	return obj;

size_align_fail:
	dma_buf_detach(dma_buf, attach);
attach_fail:
	dma_buf_put(dma_buf);
	kfree(bo);
out:
	return ERR_PTR(ret);
}

static int qaic_prepare_import_bo(struct qaic_bo *bo, struct qaic_attach_slice_hdr *hdr)
{
	struct drm_gem_object *obj = &bo->base;
	struct sg_table *sgt;
	int ret;

	sgt = dma_buf_map_attachment(obj->import_attach, hdr->dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		return ret;
	}

	bo->sgt = sgt;

	return 0;
}

static int qaic_prepare_export_bo(struct qaic_device *qdev, struct qaic_bo *bo,
				  struct qaic_attach_slice_hdr *hdr)
{
	int ret;

	ret = dma_map_sgtable(&qdev->pdev->dev, bo->sgt, hdr->dir, 0);
	if (ret)
		return -EFAULT;

	return 0;
}

static int qaic_prepare_bo(struct qaic_device *qdev, struct qaic_bo *bo,
			   struct qaic_attach_slice_hdr *hdr)
{
	int ret;

	if (drm_gem_is_imported(&bo->base))
		ret = qaic_prepare_import_bo(bo, hdr);
	else
		ret = qaic_prepare_export_bo(qdev, bo, hdr);
	bo->dir = hdr->dir;
	bo->dbc = &qdev->dbc[hdr->dbc_id];
	bo->nr_slice = hdr->count;

	return ret;
}

static void qaic_unprepare_import_bo(struct qaic_bo *bo)
{
	dma_buf_unmap_attachment(bo->base.import_attach, bo->sgt, bo->dir);
	bo->sgt = NULL;
}

static void qaic_unprepare_export_bo(struct qaic_device *qdev, struct qaic_bo *bo)
{
	dma_unmap_sgtable(&qdev->pdev->dev, bo->sgt, bo->dir, 0);
}

static void qaic_unprepare_bo(struct qaic_device *qdev, struct qaic_bo *bo)
{
	if (drm_gem_is_imported(&bo->base))
		qaic_unprepare_import_bo(bo);
	else
		qaic_unprepare_export_bo(qdev, bo);

	bo->dir = 0;
	bo->dbc = NULL;
	bo->nr_slice = 0;
	bo->sliced = false;
}

static void qaic_free_slices_bo(struct qaic_bo *bo)
{
	struct bo_slice *slice, *temp;

	list_for_each_entry_safe(slice, temp, &bo->slices, slice)
		kref_put(&slice->ref_count, free_slice);
	if (WARN_ON_ONCE(bo->total_slice_nents != 0))
		bo->total_slice_nents = 0;
	bo->nr_slice = 0;
}

static int qaic_attach_slicing_bo(struct qaic_device *qdev, struct qaic_bo *bo,
				  struct qaic_attach_slice_hdr *hdr,
				  struct qaic_attach_slice_entry *slice_ent)
{
	int ret, i;

	for (i = 0; i < hdr->count; i++) {
		ret = qaic_map_one_slice(qdev, bo, &slice_ent[i]);
		if (ret) {
			qaic_free_slices_bo(bo);
			return ret;
		}
	}

	return 0;
}

int qaic_attach_slice_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qaic_attach_slice_entry *slice_ent;
	struct qaic_attach_slice *args = data;
	int rcu_id, usr_rcu_id, qdev_rcu_id;
	struct dma_bridge_chan	*dbc;
	struct drm_gem_object *obj;
	struct qaic_device *qdev;
	unsigned long arg_size;
	struct qaic_user *usr;
	u8 __user *user_data;
	struct qaic_bo *bo;
	int ret;

	if (args->hdr.count == 0)
		return -EINVAL;

	if (check_mul_overflow((unsigned long)args->hdr.count,
			       (unsigned long)sizeof(*slice_ent),
			       &arg_size))
		return -EINVAL;

	if (!(args->hdr.dir == DMA_TO_DEVICE || args->hdr.dir == DMA_FROM_DEVICE))
		return -EINVAL;

	if (args->data == 0)
		return -EINVAL;

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	if (args->hdr.dbc_id >= qdev->num_dbc) {
		ret = -EINVAL;
		goto unlock_dev_srcu;
	}

	user_data = u64_to_user_ptr(args->data);

	slice_ent = memdup_user(user_data, arg_size);
	if (IS_ERR(slice_ent)) {
		ret = PTR_ERR(slice_ent);
		goto unlock_dev_srcu;
	}

	obj = drm_gem_object_lookup(file_priv, args->hdr.handle);
	if (!obj) {
		ret = -ENOENT;
		goto free_slice_ent;
	}

	ret = qaic_validate_req(qdev, slice_ent, args->hdr.count, obj->size);
	if (ret)
		goto put_bo;

	bo = to_qaic_bo(obj);
	ret = mutex_lock_interruptible(&bo->lock);
	if (ret)
		goto put_bo;

	if (bo->sliced) {
		ret = -EINVAL;
		goto unlock_bo;
	}

	dbc = &qdev->dbc[args->hdr.dbc_id];
	rcu_id = srcu_read_lock(&dbc->ch_lock);
	if (dbc->usr != usr) {
		ret = -EINVAL;
		goto unlock_ch_srcu;
	}

	if (dbc->id == qdev->ssr_dbc) {
		ret = -EPIPE;
		goto unlock_ch_srcu;
	}

	ret = qaic_prepare_bo(qdev, bo, &args->hdr);
	if (ret)
		goto unlock_ch_srcu;

	ret = qaic_attach_slicing_bo(qdev, bo, &args->hdr, slice_ent);
	if (ret)
		goto unprepare_bo;

	if (args->hdr.dir == DMA_TO_DEVICE)
		dma_sync_sgtable_for_cpu(&qdev->pdev->dev, bo->sgt, args->hdr.dir);

	bo->sliced = true;
	list_add_tail(&bo->bo_list, &bo->dbc->bo_lists);
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
	mutex_unlock(&bo->lock);
	kfree(slice_ent);
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);

	return 0;

unprepare_bo:
	qaic_unprepare_bo(qdev, bo);
unlock_ch_srcu:
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
unlock_bo:
	mutex_unlock(&bo->lock);
put_bo:
	drm_gem_object_put(obj);
free_slice_ent:
	kfree(slice_ent);
unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	return ret;
}

static inline u32 fifo_space_avail(u32 head, u32 tail, u32 q_size)
{
	u32 avail = head - tail - 1;

	if (head <= tail)
		avail += q_size;

	return avail;
}

static void submit_reqs_to_fifo(struct dma_bridge_chan *dbc, struct dbc_req *reqs,
			       u32 num_req, u32 head, u32 tail)
{
	u32 num_submit;

	if (tail + num_req > dbc->nelem) {
		num_submit = dbc->nelem - tail;
		memcpy(fifo_at(dbc->req_q_base, tail), reqs, sizeof(*reqs) * num_submit);
		reqs += num_submit;
		num_submit = num_req - num_submit;
		memcpy(dbc->req_q_base, reqs, sizeof(*reqs) * num_submit);
	} else {
		memcpy(fifo_at(dbc->req_q_base, tail), reqs, sizeof(*reqs) * num_req);
	}
}

/* Caller should be holding dbc->req_lock */
int qaic_submit_reqs_to_hw(struct bo_slice *slice, struct dbc_req *reqs,
			   unsigned int num_req, bool is_partial,
			   u32 partial_size, u16 job_id)
{
	struct dma_bridge_chan *dbc = slice->bo->dbc;
	struct qaic_bo *bo = slice->bo;
	struct dbc_req *last_req;
	u32 head, tail, avail;

	qaic_data_get_fifo_info(dbc, &head, &tail);
	avail = fifo_space_avail(head, tail, dbc->nelem);

	/* PCI link error */
	if (head == U32_MAX || tail == U32_MAX)
		return -ENODEV;

	if (avail < num_req)
		return -EAGAIN;

	submit_reqs_to_fifo(dbc, reqs, num_req, head, tail);

	last_req = fifo_at(dbc->req_q_base, (tail + num_req - 1) % dbc->nelem);
	if (is_partial) {
		/*
		 * Copy over the last entry. Here we need to adjust len to the left over
		 * size, and set src and dst to the entry it is copied to.
		 */
		memcpy(last_req, &slice->reqs[slice->nents - 1], sizeof(*last_req));
		last_req->len = cpu_to_le32(partial_size);
		last_req->src_addr = reqs[num_req - 1].src_addr;
		last_req->dest_addr = reqs[num_req - 1].dest_addr;
		last_req->req_id = reqs[num_req - 1].req_id;
		/* Disable DMA transfer */
		if (!last_req->len)
			last_req->cmd &= ~GENMASK(1, 0);
	}
	/* When a BO has been split into multiple jobs, each job needs to signal its completion */
	last_req->cmd |= GEN_COMPLETION;

	tail = (tail + num_req) % dbc->nelem;

	/* Submit_ts will be taken for the last job of this BO */
	bo->perf_stats.req_submit_ts = ktime_get_ns();

	/* Finalize commit to hardware */
	if (bo->need_dev_sync) {
		bo->need_dev_sync = false;
		dma_sync_sgtable_for_device(&dbc->qdev->pdev->dev, bo->sgt, bo->dir);
	}
	writel(tail, dbc->dbc_base + REQTP_OFF);
	return 0;
}

static inline void qaic_dbc_put_job(struct dma_bridge_chan *dbc, struct qaic_job *job)
{
	list_del_init(&job->queue);
	dbc->pending_reqs -= job->num_req;
	qaic_put_job(job);
}

static int create_slice_jobs(struct drm_file *file_priv, struct bo_slice *slice, bool partial,
			     u64 resize, struct list_head *tmp_list)
{
	unsigned int nents_offset, nents_xfer;
	struct dbc_req *reqs = slice->reqs;
	struct qaic_job *job, *job_i;
	unsigned int job_count = 0;
	u64 last_bytes, seq_no;
	u32 first_n;

	/*
	 * After this for loop is complete, first_n represents the index
	 * of the last DMA request of this slice that needs to be
	 * transferred after resizing and last_bytes represents DMA size
	 * of that request.
	 */
	last_bytes = resize;
	for (first_n = 0; first_n < slice->nents; first_n++)
		if (last_bytes > le32_to_cpu(reqs[first_n].len))
			last_bytes -= le32_to_cpu(reqs[first_n].len);
		else
			break;

	if (partial)
		nents_xfer = first_n + 1;
	else
		nents_xfer = slice->nents;

	nents_offset = 0;
	seq_no = 0;
	job = NULL;
	while (nents_xfer > nents_offset) {
		job = qaic_create_job(file_priv, slice, nents_offset, nents_xfer,
				      seq_no++, tmp_list);
		if (IS_ERR(job))
			goto free_jobs;

		job_count++;
		nents_offset += job->num_req;
	}

	/* job points to the last job for this slice, only valid for partial execute ioctl */
	job->partial_size = last_bytes;
	job->partial = partial;

	return job_count;

free_jobs:
	list_for_each_entry_safe_reverse(job, job_i, tmp_list, queue)
		if (job_count--)
			qaic_cleanup_job(job);
		else
			break;
	return job_count;
}

static int send_bo_list_to_sched(struct qaic_device *qdev, struct drm_file *file_priv,
				 struct execute_info *exec, struct dma_bridge_chan *dbc)
{
	struct qaic_job *job, *job_i, *tmp;
	struct ww_acquire_ctx acquire_ctx;
	struct bo_slice *slice;
	unsigned long flags;
	LIST_HEAD(tmp_list);
	struct qaic_bo *bo;
	int job_count;
	u64 resize;
	u32 handle;
	int i, j;
	int ret;

	ret = drm_gem_lock_reservations((struct drm_gem_object **)exec->bo_arr,
					exec->count, &acquire_ctx);
	if (ret)
		return ret;

	for (i = 0; i < exec->count; i++) {
		resize = exec->is_partial ? exec->resize_arr[i] : 0;
		bo = exec->bo_arr[i];
		handle = exec->handle_arr[i];
		ret = mutex_lock_interruptible(&bo->lock);
		if (ret)
			goto free_jobs;

		if (!bo->sliced) {
			ret = -EINVAL;
			goto unlock_bo;
		}

		if (exec->is_partial && resize > bo->base.size) {
			ret = -EINVAL;
			goto unlock_bo;
		}

		if (bo_queued(bo)) {
			ret = -EINVAL;
			goto unlock_bo;
		}

		job_count = 0;
		list_for_each_entry(slice, &bo->slices, slice) {
			if (exec->is_partial && (!resize || resize <= slice->offset))
				/* Configure the slice for no DMA transfer */
				ret = create_slice_jobs(file_priv, slice, true, 0, &tmp_list);
			else if (exec->is_partial && resize < slice->offset + slice->size)
				/* Configure the slice to be partially DMA transferred */
				ret = create_slice_jobs(file_priv, slice, true,
							resize - slice->offset, &tmp_list);
			else
				ret = create_slice_jobs(file_priv, slice, false, 0, &tmp_list);
			if (ret <= 0)
				goto unlock_bo;
			job_count += ret;
		}

		ret = qaic_acquire_bo_fence(bo, job_count, &tmp_list);
		if (ret)
			goto unlock_bo;

		bo->need_dev_sync = true;
		mutex_unlock(&bo->lock);
	}

	spin_lock_irqsave(&dbc->job_q_lock, flags);
	job = list_last_entry(&dbc->job_queue, typeof(*job), queue);
	list_splice_tail_init(&tmp_list, &dbc->job_queue);
	list_for_each_entry_continue(job, &dbc->job_queue, queue)
		qaic_submit_job(dbc, job);
	spin_unlock_irqrestore(&dbc->job_q_lock, flags);

	goto unlock_resv;

unlock_bo:
	mutex_unlock(&bo->lock);
free_jobs:
	for (j = i - 1; i > 0 && j >= 0; j--) {
		bo = exec->bo_arr[j];
		ret = mutex_lock_interruptible(&bo->lock);
		if (!ret) {
			bo->need_dev_sync = false;
			dma_fence_put(bo->fence);
			bo->fence = NULL;
			mutex_unlock(&bo->lock);
		}
	}
	list_for_each_entry_safe_reverse(job_i, tmp, &tmp_list, queue)
		qaic_cleanup_job(job_i);
unlock_resv:
	drm_gem_unlock_reservations((struct drm_gem_object **)exec->bo_arr,
				    exec->count, &acquire_ctx);
	return ret;
}

static void update_profiling_data(struct execute_info *exec)
{
	u32 queue_level = exec->perf_stats.queue_level;
	struct qaic_bo *bo;
	int i;

	for (i = 0; i < exec->count; i++) {
		bo = exec->bo_arr[i];
		bo->perf_stats.req_received_ts = exec->perf_stats.received_ts;
		bo->perf_stats.req_submit_ts = exec->perf_stats.submit_ts;
		bo->perf_stats.queue_level_before = queue_level;
		queue_level += bo->total_slice_nents;
	}
}

static int exec_index_to_gem_info(struct qaic_exec_data_req req, struct qaic_exec_data_ent *ent)
{
	int idx = req.idx;
	int ret = 0;
	u32 handle;

	if (req.is_partial) {
		struct qaic_partial_execute_entry *exec_ent_part = req.exec_data;

		handle = exec_ent_part[idx].handle;
		ent->resize = exec_ent_part[idx].resize;
	} else {
		struct qaic_execute_entry *exec_ent = req.exec_data;

		handle = exec_ent[idx].handle;
	}

	ent->obj = drm_gem_object_lookup(req.drm_file, handle);
	if (!ent->obj)
		ret = -ENOENT;

	ent->handle = handle;

	return ret;
}

static int lookup_exec_data(struct drm_file *file_priv, void *exec_data,
			       struct execute_info *exec)
{
	struct qaic_exec_data_req req;
	struct qaic_exec_data_ent ent;
	int i, ret = 0;

	req.drm_file = file_priv;
	req.exec_data = exec_data;
	req.is_partial = exec->is_partial;

	for (i = 0; i < exec->count; i++) {
		req.idx = i;
		ret = exec_index_to_gem_info(req, &ent);
		if (ret)
			goto put_obj;

		exec->bo_arr[i] = to_qaic_bo(ent.obj);
		exec->handle_arr[i] = ent.handle;
		if (exec->resize_arr)
			exec->resize_arr[i] = ent.resize;
	}

	return ret;
put_obj:
	for (i--; i >= 0; i--)
		drm_gem_object_put(&exec->bo_arr[i]->base);
	return ret;
}

static int __qaic_execute_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv,
				   bool is_partial)
{
	size_t usr_ent_size = is_partial ? sizeof(struct qaic_partial_execute_entry) :
					   sizeof(struct qaic_execute_entry);
	int usr_rcu_id, qdev_rcu_id, ch_rcu_id;
	struct qaic_execute *args = data;
	struct dma_bridge_chan *dbc;
	struct qaic_device *qdev;
	struct execute_info exec;
	struct qaic_user *usr;
	size_t elem_size_sum;
	void *usr_exec_ent;
	int i, ret;

	exec.perf_stats.received_ts = ktime_get_ns();
	exec.is_partial = is_partial;

	if (args->hdr.count == 0)
		return -EINVAL;

	exec.count = args->hdr.count;

	usr_exec_ent = memdup_array_user(u64_to_user_ptr(args->data), exec.count, usr_ent_size);
	if (IS_ERR(usr_exec_ent))
		return PTR_ERR(usr_exec_ent);

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	if (args->hdr.dbc_id >= qdev->num_dbc) {
		ret = -EINVAL;
		goto unlock_dev_srcu;
	}

	dbc = &qdev->dbc[args->hdr.dbc_id];

	/*
	 * Going to allocate one large array, and then create pointers to
	 * within it for sub-arrays that all have exec.count elements in them.
	 *
	 * bo_arr is an array of pointers to struct qaic_bo, but is used as an
	 * argument for drm_gem_lock_reservations().
	 *
	 * handle_arr is an array of handles to each drm_gem_object embedded in
	 * the respective bo_arr element. It is used to help debugging by
	 * identifying particular buffers.
	 *
	 * resize_arr is an array of u64 sizes for partial executions. It is
	 * only defined if a partial execution is underway.
	 */
	elem_size_sum = sizeof(*exec.bo_arr) + sizeof(*exec.handle_arr);
	elem_size_sum += is_partial ? sizeof(*exec.resize_arr) : 0;
	exec.bo_arr = kcalloc(exec.count, elem_size_sum, GFP_KERNEL);
	if (!exec.bo_arr) {
		ret = -ENOMEM;
		goto unlock_dev_srcu;
	}
	exec.handle_arr = ((void *)exec.bo_arr) + (sizeof(*exec.bo_arr) * exec.count);
	if (is_partial)
		exec.resize_arr = ((void *)exec.bo_arr) +
				  ((sizeof(*exec.bo_arr) + sizeof(*exec.handle_arr)) * exec.count);
	else
		exec.resize_arr = NULL;

	ret = lookup_exec_data(file_priv, usr_exec_ent, &exec);
	if (ret)
		goto free_bo_arr;

	ch_rcu_id = srcu_read_lock(&dbc->ch_lock);
	if (!dbc->usr || dbc->usr->handle != usr->handle) {
		ret = -EPERM;
		goto release_ch_rcu;
	}

	if (dbc->id == qdev->ssr_dbc) {
		ret = -EPIPE;
		goto release_ch_rcu;
	}

	/* Not locking to read pending_reqs since exact value not necessary */
	exec.perf_stats.queue_level = dbc->pending_reqs;

	ret = send_bo_list_to_sched(qdev, file_priv, &exec, dbc);
	if (ret)
		goto release_ch_rcu;

	exec.perf_stats.submit_ts = ktime_get_ns();

	update_profiling_data(&exec);

	if (datapath_polling)
		schedule_work(&dbc->poll_work);

release_ch_rcu:
	srcu_read_unlock(&dbc->ch_lock, ch_rcu_id);
	for (i = 0; i < exec.count; i++)
		drm_gem_object_put(&exec.bo_arr[i]->base);
free_bo_arr:
	kfree(exec.bo_arr);
unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	kfree(usr_exec_ent);
	return ret;
}

int qaic_execute_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return __qaic_execute_bo_ioctl(dev, data, file_priv, false);
}

int qaic_partial_execute_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return __qaic_execute_bo_ioctl(dev, data, file_priv, true);
}

/*
 * Our interrupt handling is a bit more complicated than a simple ideal, but
 * sadly necessary.
 *
 * Each dbc has a completion queue. Entries in the queue correspond to DMA
 * requests which the device has processed. The hardware already has a built
 * in irq mitigation. When the device puts an entry into the queue, it will
 * only trigger an interrupt if the queue was empty. Therefore, when adding
 * the Nth event to a non-empty queue, the hardware doesn't trigger an
 * interrupt. This means the host doesn't get additional interrupts signaling
 * the same thing - the queue has something to process.
 * This behavior can be overridden in the DMA request.
 * This means that when the host receives an interrupt, it is required to
 * drain the queue.
 *
 * This behavior is what NAPI attempts to accomplish, although we can't use
 * NAPI as we don't have a netdev. We use threaded irqs instead.
 *
 * However, there is a situation where the host drains the queue fast enough
 * that every event causes an interrupt. Typically this is not a problem as
 * the rate of events would be low. However, that is not the case with
 * lprnet for example. On an Intel Xeon D-2191 where we run 8 instances of
 * lprnet, the host receives roughly 80k interrupts per second from the device
 * (per /proc/interrupts). While NAPI documentation indicates the host should
 * just chug along, sadly that behavior causes instability in some hosts.
 *
 * Therefore, we implement an interrupt disable scheme similar to NAPI. The
 * key difference is that we will delay after draining the queue for a small
 * time to allow additional events to come in via polling. Using the above
 * lprnet workload, this reduces the number of interrupts processed from
 * ~80k/sec to about 64 in 5 minutes and appears to solve the system
 * instability.
 */
irqreturn_t dbc_irq_handler(int irq, void *data)
{
	struct dma_bridge_chan *dbc = data;
	int rcu_id;
	u32 head;
	u32 tail;

	rcu_id = srcu_read_lock(&dbc->ch_lock);

	if (datapath_polling) {
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		/*
		 * Normally datapath_polling will not have irqs enabled, but
		 * when running with only one MSI the interrupt is shared with
		 * MHI so it cannot be disabled. Return ASAP instead.
		 */
		return IRQ_HANDLED;
	}

	if (!dbc->usr) {
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_HANDLED;
	}

	head = readl(dbc->dbc_base + RSPHP_OFF);
	if (head == U32_MAX) { /* PCI link error */
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_NONE;
	}

	tail = readl(dbc->dbc_base + RSPTP_OFF);
	if (tail == U32_MAX) { /* PCI link error */
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_NONE;
	}

	if (head == tail) { /* queue empty */
		srcu_read_unlock(&dbc->ch_lock, rcu_id);
		return IRQ_NONE;
	}

	if (!dbc->qdev->single_msi)
		disable_irq_nosync(irq);
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
	return IRQ_WAKE_THREAD;
}

void qaic_irq_polling_work(struct work_struct *work)
{
	struct dma_bridge_chan *dbc = container_of(work, struct dma_bridge_chan,  poll_work);
	unsigned long flags;
	int rcu_id;
	u32 head;
	u32 tail;

	rcu_id = srcu_read_lock(&dbc->ch_lock);

	while (1) {
		if (dbc->qdev->dev_state != QAIC_ONLINE) {
			srcu_read_unlock(&dbc->ch_lock, rcu_id);
			return;
		}
		if (!dbc->usr) {
			srcu_read_unlock(&dbc->ch_lock, rcu_id);
			return;
		}
		spin_lock_irqsave(&dbc->job_q_lock, flags);
		if (list_empty(&dbc->job_queue)) {
			spin_unlock_irqrestore(&dbc->job_q_lock, flags);
			srcu_read_unlock(&dbc->ch_lock, rcu_id);
			return;
		}
		spin_unlock_irqrestore(&dbc->job_q_lock, flags);

		head = readl(dbc->dbc_base + RSPHP_OFF);
		if (head == U32_MAX) { /* PCI link error */
			srcu_read_unlock(&dbc->ch_lock, rcu_id);
			return;
		}

		tail = readl(dbc->dbc_base + RSPTP_OFF);
		if (tail == U32_MAX) { /* PCI link error */
			srcu_read_unlock(&dbc->ch_lock, rcu_id);
			return;
		}

		if (head != tail) {
			irq_wake_thread(dbc->irq, dbc);
			srcu_read_unlock(&dbc->ch_lock, rcu_id);
			return;
		}

		cond_resched();
		usleep_range(datapath_poll_interval_us, 2 * datapath_poll_interval_us);
	}
}

irqreturn_t dbc_irq_threaded_fn(int irq, void *data)
{
	struct dma_bridge_chan *dbc = data;
	int event_count = NUM_EVENTS;
	int delay_count = NUM_DELAYS;
	struct qaic_job *job, *tmp;
	struct qaic_device *qdev;
	struct dbc_rsp *rsp;
	unsigned long flags;
	int rcu_id;
	u16 status;
	u16 req_id;
	u32 head;
	u32 tail;

	rcu_id = srcu_read_lock(&dbc->ch_lock);
	qdev = dbc->qdev;

	head = readl(dbc->dbc_base + RSPHP_OFF);
	if (head == U32_MAX) /* PCI link error */
		goto error_out;

read_fifo:

	if (!event_count) {
		event_count = NUM_EVENTS;
		cond_resched();
	}

	/*
	 * if this channel isn't assigned or gets unassigned during processing
	 * we have nothing further to do
	 */
	if (!dbc->usr)
		goto error_out;

	tail = readl(dbc->dbc_base + RSPTP_OFF);
	if (tail == U32_MAX) /* PCI link error */
		goto error_out;

	if (head == tail) { /* queue empty */
		if (delay_count) {
			--delay_count;
			usleep_range(100, 200);
			goto read_fifo; /* check for a new event */
		}
		goto normal_out;
	}

	delay_count = NUM_DELAYS;
	while (head != tail) {
		if (!event_count)
			break;
		--event_count;
		rsp = dbc->rsp_q_base + head * sizeof(*rsp);
		req_id = le16_to_cpu(rsp->req_id);
		status = le16_to_cpu(rsp->status);
		if (status)
			pci_dbg(qdev->pdev, "req_id %d failed with status %d\n", req_id, status);

		spin_lock_irqsave(&dbc->job_q_lock, flags);
		list_for_each_entry_safe(job, tmp, &dbc->job_queue, queue) {
			if (job->id == req_id) {
				dma_fence_signal(&job->irq_fence->base);
				qaic_dbc_put_job(dbc, job);
				break;
			}
		}
		spin_unlock_irqrestore(&dbc->job_q_lock, flags);

		head = (head + 1) % dbc->nelem;
	}

	/*
	 * Update the head pointer of response queue and let the device know
	 * that we have consumed elements from the queue.
	 */
	writel(head, dbc->dbc_base + RSPHP_OFF);

	/* elements might have been put in the queue while we were processing */
	goto read_fifo;

normal_out:
	if (!qdev->single_msi && likely(!datapath_polling))
		enable_irq(irq);
	else if (unlikely(datapath_polling))
		schedule_work(&dbc->poll_work);
	/* checking the fifo and enabling irqs is a race, missed event check */
	tail = readl(dbc->dbc_base + RSPTP_OFF);
	if (tail != U32_MAX && head != tail) {
		if (!qdev->single_msi && likely(!datapath_polling))
			disable_irq_nosync(irq);
		goto read_fifo;
	}
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
	return IRQ_HANDLED;

error_out:
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
	if (!qdev->single_msi && likely(!datapath_polling))
		enable_irq(irq);
	else if (unlikely(datapath_polling))
		schedule_work(&dbc->poll_work);

	return IRQ_HANDLED;
}

int qaic_wait_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qaic_wait *args = data;
	int usr_rcu_id, qdev_rcu_id;
	struct dma_bridge_chan *dbc;
	struct drm_gem_object *obj;
	struct qaic_device *qdev;
	struct dma_fence *fence;
	unsigned long timeout;
	struct qaic_user *usr;
	struct qaic_bo *bo;
	int rcu_id;
	int ret;

	if (args->pad != 0)
		return -EINVAL;

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	if (args->dbc_id >= qdev->num_dbc) {
		ret = -EINVAL;
		goto unlock_dev_srcu;
	}

	dbc = &qdev->dbc[args->dbc_id];

	rcu_id = srcu_read_lock(&dbc->ch_lock);
	if (dbc->usr != usr) {
		ret = -EPERM;
		goto unlock_ch_srcu;
	}

	if (dbc->id == qdev->ssr_dbc) {
		ret = -EPIPE;
		goto unlock_ch_srcu;
	}

	timeout = args->timeout ? args->timeout : wait_exec_default_timeout_ms;
	timeout = msecs_to_jiffies(timeout);

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock_ch_srcu;
	}

	bo = to_qaic_bo(obj);
	ret = mutex_lock_interruptible(&bo->lock);
	if (ret)
		goto put_obj;

	if (!bo->fence) {
		ret = -ENOENT;
		mutex_unlock(&bo->lock);
		goto put_obj;
	}

	fence = bo->fence;
	dma_fence_get(fence);
	mutex_unlock(&bo->lock);

	ret = qaic_fence_wait(fence, timeout);
	dma_fence_put(fence);
	if (ret)
		goto put_obj;

	if (!dbc->usr)
		ret = -EPERM;

	if (dbc->id == qdev->ssr_dbc)
		ret = -EPIPE;

put_obj:
	drm_gem_object_put(obj);
unlock_ch_srcu:
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	return ret;
}

int qaic_perf_stats_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qaic_perf_stats_entry *ent = NULL;
	struct qaic_perf_stats *args = data;
	int usr_rcu_id, qdev_rcu_id;
	struct drm_gem_object *obj;
	struct qaic_device *qdev;
	struct qaic_user *usr;
	struct qaic_bo *bo;
	int ret = 0;
	int i;

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	if (args->hdr.dbc_id >= qdev->num_dbc) {
		ret = -EINVAL;
		goto unlock_dev_srcu;
	}

	ent = memdup_array_user(u64_to_user_ptr(args->data), args->hdr.count, sizeof(*ent));
	if (IS_ERR(ent)) {
		ret = PTR_ERR(ent);
		goto unlock_dev_srcu;
	}

	for (i = 0; i < args->hdr.count; i++) {
		obj = drm_gem_object_lookup(file_priv, ent[i].handle);
		if (!obj) {
			ret = -ENOENT;
			goto free_ent;
		}
		bo = to_qaic_bo(obj);
		if (!bo->sliced) {
			drm_gem_object_put(obj);
			ret = -EINVAL;
			goto free_ent;
		}
		if (bo->dbc->id != args->hdr.dbc_id) {
			drm_gem_object_put(obj);
			ret = -EINVAL;
			goto free_ent;
		}
		/*
		 * perf stats ioctl is called before wait ioctl is complete then
		 * the latency information is invalid.
		 */
		if (bo->perf_stats.req_processed_ts < bo->perf_stats.req_submit_ts) {
			ent[i].device_latency_us = 0;
		} else {
			ent[i].device_latency_us = div_u64((bo->perf_stats.req_processed_ts -
							    bo->perf_stats.req_submit_ts), 1000);
		}
		ent[i].submit_latency_us = div_u64((bo->perf_stats.req_submit_ts -
						    bo->perf_stats.req_received_ts), 1000);
		ent[i].queue_level_before = bo->perf_stats.queue_level_before;
		ent[i].num_queue_element = bo->total_slice_nents;
		drm_gem_object_put(obj);
	}

	if (copy_to_user(u64_to_user_ptr(args->data), ent, args->hdr.count * sizeof(*ent)))
		ret = -EFAULT;

free_ent:
	kfree(ent);
unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	return ret;
}

static void detach_slice_bo(struct qaic_device *qdev, struct qaic_bo *bo)
{
	qaic_remove_bo_fence(bo);
	qaic_free_slices_bo(bo);
	qaic_unprepare_bo(qdev, bo);
	qaic_init_bo_lists(bo);
	list_del(&bo->bo_list);
	drm_gem_object_put(&bo->base);
}

int qaic_detach_slice_bo_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qaic_detach_slice *args = data;
	int rcu_id, usr_rcu_id, qdev_rcu_id;
	struct dma_bridge_chan *dbc;
	struct drm_gem_object *obj;
	struct qaic_device *qdev;
	struct qaic_user *usr;
	struct qaic_bo *bo;
	int ret;

	if (args->pad != 0)
		return -EINVAL;

	usr = file_priv->driver_priv;
	usr_rcu_id = srcu_read_lock(&usr->qddev_lock);
	if (!usr->qddev) {
		ret = -ENODEV;
		goto unlock_usr_srcu;
	}

	qdev = usr->qddev->qdev;
	qdev_rcu_id = srcu_read_lock(&qdev->dev_lock);
	if (qdev->dev_state != QAIC_ONLINE) {
		ret = -ENODEV;
		goto unlock_dev_srcu;
	}

	obj = drm_gem_object_lookup(file_priv, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto unlock_dev_srcu;
	}

	bo = to_qaic_bo(obj);
	ret = mutex_lock_interruptible(&bo->lock);
	if (ret)
		goto put_bo;

	if (!bo->sliced) {
		ret = -EINVAL;
		goto unlock_bo;
	}

	dbc = bo->dbc;
	rcu_id = srcu_read_lock(&dbc->ch_lock);
	if (dbc->usr != usr) {
		ret = -EINVAL;
		goto unlock_ch_srcu;
	}

	/* Check if BO is committed to H/W for DMA */
	if (bo_queued(bo)) {
		ret = -EBUSY;
		goto unlock_ch_srcu;
	}

	detach_slice_bo(qdev, bo);

unlock_ch_srcu:
	srcu_read_unlock(&dbc->ch_lock, rcu_id);
unlock_bo:
	mutex_unlock(&bo->lock);
put_bo:
	drm_gem_object_put(obj);
unlock_dev_srcu:
	srcu_read_unlock(&qdev->dev_lock, qdev_rcu_id);
unlock_usr_srcu:
	srcu_read_unlock(&usr->qddev_lock, usr_rcu_id);
	return ret;
}

static void empty_xfer_list(struct dma_bridge_chan *dbc)
{
	struct qaic_job *job, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&dbc->job_q_lock, flags);
	list_for_each_entry_safe_reverse(job, tmp, &dbc->job_queue, queue) {
		dma_fence_get(&job->irq_fence->base);
		dma_fence_set_error(&job->irq_fence->base, -ECANCELED);
		dma_fence_signal(&job->irq_fence->base);
		dma_fence_put(&job->irq_fence->base);
		qaic_dbc_put_job(dbc, job);
	}
	spin_unlock_irqrestore(&dbc->job_q_lock, flags);
}

static void sync_empty_xfer_list(struct dma_bridge_chan *dbc)
{
	empty_xfer_list(dbc);
	synchronize_srcu(&dbc->ch_lock);
	/*
	 * Threads holding channel lock, may add more elements in the xfer_list.
	 * Flush out these elements from xfer_list.
	 */
	empty_xfer_list(dbc);
}

int disable_dbc(struct qaic_device *qdev, u32 dbc_id, struct qaic_user *usr)
{
	if (!qdev->dbc[dbc_id].usr || qdev->dbc[dbc_id].usr->handle != usr->handle)
		return -EPERM;

	qdev->dbc[dbc_id].usr = NULL;
	synchronize_srcu(&qdev->dbc[dbc_id].ch_lock);
	return 0;
}

/**
 * enable_dbc - Enable the DBC. DBCs are disabled by removing the context of
 * user. Add user context back to DBC to enable it. This function trusts the
 * DBC ID passed and expects the DBC to be disabled.
 * @qdev: qaic device handle
 * @dbc_id: ID of the DBC
 * @usr: User context
 */
void enable_dbc(struct qaic_device *qdev, u32 dbc_id, struct qaic_user *usr)
{
	qaic_sched_entity_init(&qdev->dbc[dbc_id]);
	set_dbc_scaling_ratio(&qdev->dbc[dbc_id], qdev->dbc[dbc_id].nelem);
	qdev->dbc[dbc_id].usr = usr;
}

void wakeup_dbc(struct qaic_device *qdev, u32 dbc_id)
{
	struct dma_bridge_chan *dbc = &qdev->dbc[dbc_id];

	dbc->usr = NULL;
	sync_empty_xfer_list(dbc);
}

void release_dbc(struct qaic_device *qdev, u32 dbc_id)
{
	struct qaic_bo *bo, *bo_temp;
	struct dma_bridge_chan *dbc;

	dbc = &qdev->dbc[dbc_id];
	if (!dbc->in_use)
		return;

	wakeup_dbc(qdev, dbc_id);
	drm_sched_entity_destroy(&dbc->sched_entity);

	dma_free_coherent(&qdev->pdev->dev, dbc->total_size, dbc->req_q_base, dbc->dma_addr);
	dbc->total_size = 0;
	dbc->req_q_base = NULL;
	dbc->dma_addr = 0;
	dbc->nelem = 0;
	dbc->usr = NULL;

	list_for_each_entry_safe(bo, bo_temp, &dbc->bo_lists, bo_list) {
		drm_gem_object_get(&bo->base);
		mutex_lock(&bo->lock);
		detach_slice_bo(qdev, bo);
		mutex_unlock(&bo->lock);
		drm_gem_object_put(&bo->base);
	}

	dbc->in_use = false;
	wake_up(&dbc->dbc_release);
}

void qaic_data_get_fifo_info(struct dma_bridge_chan *dbc, u32 *head, u32 *tail)
{
	if (!dbc || !head || !tail)
		return;

	*head = readl(dbc->dbc_base + REQHP_OFF);
	*tail = readl(dbc->dbc_base + REQTP_OFF);
}

/*
 * qaic_dbc_enter_ssr - Prepare to enter in sub system reset(SSR) for given DBC ID.
 * @qdev: qaic device handle
 * @dbc_id: ID of the DBC which will enter SSR
 *
 * The device will automatically deactivate the workload as not
 * all errors can be silently recovered. The user will be
 * notified and will need to decide the required recovery
 * action to take.
 */
void qaic_dbc_enter_ssr(struct qaic_device *qdev, u32 dbc_id)
{
	qdev->ssr_dbc = dbc_id;
	release_dbc(qdev, dbc_id);
}

/*
 * qaic_dbc_exit_ssr - Prepare to exit from sub system reset(SSR) for given DBC ID.
 * @qdev: qaic device handle
 *
 * The DBC returns to an operational state and begins accepting work after exiting SSR.
 */
void qaic_dbc_exit_ssr(struct qaic_device *qdev)
{
	qdev->ssr_dbc = QAIC_SSR_DBC_SENTINEL;
}
