// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Copyright (c) 2025, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * Description: Direct Verbs interpreter
 */

#include <rdma/ib_addr.h>
#include <rdma/uverbs_types.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/ib_user_ioctl_cmds.h>
#define UVERBS_MODULE_NAME bnxt_re
#include <rdma/uverbs_named_ioctl.h>
#include <rdma/ib_umem.h>
#include <rdma/bnxt_re-abi.h>

#include "roce_hsi.h"
#include "qplib_res.h"
#include "qplib_sp.h"
#include "qplib_fp.h"
#include "qplib_rcfw.h"
#include "bnxt_re.h"
#include "ib_verbs.h"

static struct bnxt_re_cq *bnxt_re_search_for_cq(struct bnxt_re_dev *rdev, u32 cq_id)
{
	struct bnxt_re_cq *cq = NULL, *tmp_cq;

	hash_for_each_possible(rdev->cq_hash, tmp_cq, hash_entry, cq_id) {
		if (tmp_cq->qplib_cq.id == cq_id) {
			cq = tmp_cq;
			break;
		}
	}
	return cq;
}

static struct bnxt_re_srq *bnxt_re_search_for_srq(struct bnxt_re_dev *rdev, u32 srq_id)
{
	struct bnxt_re_srq *srq = NULL, *tmp_srq;

	hash_for_each_possible(rdev->srq_hash, tmp_srq, hash_entry, srq_id) {
		if (tmp_srq->qplib_srq.id == srq_id) {
			srq = tmp_srq;
			break;
		}
	}
	return srq;
}

static int UVERBS_HANDLER(BNXT_RE_METHOD_NOTIFY_DRV)(struct uverbs_attr_bundle *attrs)
{
	struct bnxt_re_ucontext *uctx;
	struct ib_ucontext *ib_uctx;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	if (IS_ERR(uctx))
		return PTR_ERR(uctx);

	bnxt_re_pacing_alert(uctx->rdev);
	return 0;
}

static int UVERBS_HANDLER(BNXT_RE_METHOD_ALLOC_PAGE)(struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj = uverbs_attr_get_uobject(attrs, BNXT_RE_ALLOC_PAGE_HANDLE);
	enum bnxt_re_alloc_page_type alloc_type;
	struct bnxt_re_user_mmap_entry *entry;
	enum bnxt_re_mmap_flag mmap_flag;
	struct bnxt_qplib_chip_ctx *cctx;
	struct bnxt_re_ucontext *uctx;
	struct ib_ucontext *ib_uctx;
	struct bnxt_re_dev *rdev;
	u64 mmap_offset;
	u32 length;
	u32 dpi;
	u64 addr;
	int err;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	if (IS_ERR(uctx))
		return PTR_ERR(uctx);

	err = uverbs_get_const(&alloc_type, attrs, BNXT_RE_ALLOC_PAGE_TYPE);
	if (err)
		return err;

	rdev = uctx->rdev;
	cctx = rdev->chip_ctx;

	switch (alloc_type) {
	case BNXT_RE_ALLOC_WC_PAGE:
		if (cctx->modes.db_push)  {
			if (bnxt_qplib_alloc_dpi(&rdev->qplib_res, &uctx->wcdpi,
						 uctx, BNXT_QPLIB_DPI_TYPE_WC))
				return -ENOMEM;
			length = PAGE_SIZE;
			dpi = uctx->wcdpi.dpi;
			addr = (u64)uctx->wcdpi.umdbr;
			mmap_flag = BNXT_RE_MMAP_WC_DB;
		} else {
			return -EINVAL;
		}

		break;
	case BNXT_RE_ALLOC_DBR_BAR_PAGE:
		length = PAGE_SIZE;
		addr = (u64)rdev->pacing.dbr_bar_addr;
		mmap_flag = BNXT_RE_MMAP_DBR_BAR;
		break;

	case BNXT_RE_ALLOC_DBR_PAGE:
		length = PAGE_SIZE;
		addr = (u64)rdev->pacing.dbr_page;
		mmap_flag = BNXT_RE_MMAP_DBR_PAGE;
		break;

	default:
		return -EOPNOTSUPP;
	}

	entry = bnxt_re_mmap_entry_insert(uctx, addr, mmap_flag, &mmap_offset);
	if (!entry)
		return -ENOMEM;

	uobj->object = entry;
	uverbs_finalize_uobj_create(attrs, BNXT_RE_ALLOC_PAGE_HANDLE);
	err = uverbs_copy_to(attrs, BNXT_RE_ALLOC_PAGE_MMAP_OFFSET,
			     &mmap_offset, sizeof(mmap_offset));
	if (err)
		return err;

	err = uverbs_copy_to(attrs, BNXT_RE_ALLOC_PAGE_MMAP_LENGTH,
			     &length, sizeof(length));
	if (err)
		return err;

	err = uverbs_copy_to(attrs, BNXT_RE_ALLOC_PAGE_DPI,
			     &dpi, sizeof(dpi));
	if (err)
		return err;

	return 0;
}

static int alloc_page_obj_cleanup(struct ib_uobject *uobject,
				  enum rdma_remove_reason why,
			    struct uverbs_attr_bundle *attrs)
{
	struct  bnxt_re_user_mmap_entry *entry = uobject->object;
	struct bnxt_re_ucontext *uctx = entry->uctx;

	switch (entry->mmap_flag) {
	case BNXT_RE_MMAP_WC_DB:
		if (uctx && uctx->wcdpi.dbr) {
			struct bnxt_re_dev *rdev = uctx->rdev;

			bnxt_qplib_dealloc_dpi(&rdev->qplib_res, &uctx->wcdpi);
			uctx->wcdpi.dbr = NULL;
		}
		break;
	case BNXT_RE_MMAP_DBR_BAR:
	case BNXT_RE_MMAP_DBR_PAGE:
		break;
	default:
		goto exit;
	}
	rdma_user_mmap_entry_remove(&entry->rdma_entry);
exit:
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_ALLOC_PAGE,
			    UVERBS_ATTR_IDR(BNXT_RE_ALLOC_PAGE_HANDLE,
					    BNXT_RE_OBJECT_ALLOC_PAGE,
					    UVERBS_ACCESS_NEW,
					    UA_MANDATORY),
			    UVERBS_ATTR_CONST_IN(BNXT_RE_ALLOC_PAGE_TYPE,
						 enum bnxt_re_alloc_page_type,
						 UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_ALLOC_PAGE_MMAP_OFFSET,
						UVERBS_ATTR_TYPE(u64),
						UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_ALLOC_PAGE_MMAP_LENGTH,
						UVERBS_ATTR_TYPE(u32),
						UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_ALLOC_PAGE_DPI,
						UVERBS_ATTR_TYPE(u32),
						UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(BNXT_RE_METHOD_DESTROY_PAGE,
				    UVERBS_ATTR_IDR(BNXT_RE_DESTROY_PAGE_HANDLE,
						    BNXT_RE_OBJECT_ALLOC_PAGE,
						    UVERBS_ACCESS_DESTROY,
						    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(BNXT_RE_OBJECT_ALLOC_PAGE,
			    UVERBS_TYPE_ALLOC_IDR(alloc_page_obj_cleanup),
			    &UVERBS_METHOD(BNXT_RE_METHOD_ALLOC_PAGE),
			    &UVERBS_METHOD(BNXT_RE_METHOD_DESTROY_PAGE));

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_NOTIFY_DRV);

DECLARE_UVERBS_GLOBAL_METHODS(BNXT_RE_OBJECT_NOTIFY_DRV,
			      &UVERBS_METHOD(BNXT_RE_METHOD_NOTIFY_DRV));

/* Toggle MEM */
static int UVERBS_HANDLER(BNXT_RE_METHOD_GET_TOGGLE_MEM)(struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj = uverbs_attr_get_uobject(attrs, BNXT_RE_TOGGLE_MEM_HANDLE);
	enum bnxt_re_mmap_flag mmap_flag = BNXT_RE_MMAP_TOGGLE_PAGE;
	enum bnxt_re_get_toggle_mem_type res_type;
	struct bnxt_re_user_mmap_entry *entry;
	struct bnxt_re_ucontext *uctx;
	struct ib_ucontext *ib_uctx;
	struct bnxt_re_dev *rdev;
	struct bnxt_re_srq *srq;
	u32 length = PAGE_SIZE;
	struct bnxt_re_cq *cq;
	u64 mem_offset;
	u32 offset = 0;
	u64 addr = 0;
	u32 res_id;
	int err;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	err = uverbs_get_const(&res_type, attrs, BNXT_RE_TOGGLE_MEM_TYPE);
	if (err)
		return err;

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	rdev = uctx->rdev;
	err = uverbs_copy_from(&res_id, attrs, BNXT_RE_TOGGLE_MEM_RES_ID);
	if (err)
		return err;

	switch (res_type) {
	case BNXT_RE_CQ_TOGGLE_MEM:
		cq = bnxt_re_search_for_cq(rdev, res_id);
		if (!cq)
			return -EINVAL;

		addr = (u64)cq->uctx_cq_page;
		break;
	case BNXT_RE_SRQ_TOGGLE_MEM:
		srq = bnxt_re_search_for_srq(rdev, res_id);
		if (!srq)
			return -EINVAL;

		addr = (u64)srq->uctx_srq_page;
		break;

	default:
		return -EOPNOTSUPP;
	}

	entry = bnxt_re_mmap_entry_insert(uctx, addr, mmap_flag, &mem_offset);
	if (!entry)
		return -ENOMEM;

	uobj->object = entry;
	uverbs_finalize_uobj_create(attrs, BNXT_RE_TOGGLE_MEM_HANDLE);
	err = uverbs_copy_to(attrs, BNXT_RE_TOGGLE_MEM_MMAP_PAGE,
			     &mem_offset, sizeof(mem_offset));
	if (err)
		return err;

	err = uverbs_copy_to(attrs, BNXT_RE_TOGGLE_MEM_MMAP_LENGTH,
			     &length, sizeof(length));
	if (err)
		return err;

	err = uverbs_copy_to(attrs, BNXT_RE_TOGGLE_MEM_MMAP_OFFSET,
			     &offset, sizeof(offset));
	if (err)
		return err;

	return 0;
}

static int get_toggle_mem_obj_cleanup(struct ib_uobject *uobject,
				      enum rdma_remove_reason why,
				      struct uverbs_attr_bundle *attrs)
{
	struct  bnxt_re_user_mmap_entry *entry = uobject->object;

	rdma_user_mmap_entry_remove(&entry->rdma_entry);
	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_GET_TOGGLE_MEM,
			    UVERBS_ATTR_IDR(BNXT_RE_TOGGLE_MEM_HANDLE,
					    BNXT_RE_OBJECT_GET_TOGGLE_MEM,
					    UVERBS_ACCESS_NEW,
					    UA_MANDATORY),
			    UVERBS_ATTR_CONST_IN(BNXT_RE_TOGGLE_MEM_TYPE,
						 enum bnxt_re_get_toggle_mem_type,
						 UA_MANDATORY),
			    UVERBS_ATTR_PTR_IN(BNXT_RE_TOGGLE_MEM_RES_ID,
					       UVERBS_ATTR_TYPE(u32),
					       UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_TOGGLE_MEM_MMAP_PAGE,
						UVERBS_ATTR_TYPE(u64),
						UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_TOGGLE_MEM_MMAP_OFFSET,
						UVERBS_ATTR_TYPE(u32),
						UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_TOGGLE_MEM_MMAP_LENGTH,
						UVERBS_ATTR_TYPE(u32),
						UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(BNXT_RE_METHOD_RELEASE_TOGGLE_MEM,
				    UVERBS_ATTR_IDR(BNXT_RE_RELEASE_TOGGLE_MEM_HANDLE,
						    BNXT_RE_OBJECT_GET_TOGGLE_MEM,
						    UVERBS_ACCESS_DESTROY,
						    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(BNXT_RE_OBJECT_GET_TOGGLE_MEM,
			    UVERBS_TYPE_ALLOC_IDR(get_toggle_mem_obj_cleanup),
			    &UVERBS_METHOD(BNXT_RE_METHOD_GET_TOGGLE_MEM),
			    &UVERBS_METHOD(BNXT_RE_METHOD_RELEASE_TOGGLE_MEM));

static int UVERBS_HANDLER(BNXT_RE_METHOD_DBR_ALLOC)(struct uverbs_attr_bundle *attrs)
{
	struct bnxt_re_dv_db_region dbr = {};
	struct bnxt_re_ucontext *uctx;
	struct bnxt_re_dbr_obj *obj;
	struct ib_ucontext *ib_uctx;
	struct bnxt_qplib_dpi *dpi;
	struct bnxt_re_dev *rdev;
	struct ib_uobject *uobj;
	u64 mmap_offset;
	int ret;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	rdev = uctx->rdev;
	uobj = uverbs_attr_get_uobject(attrs, BNXT_RE_DV_ALLOC_DBR_HANDLE);

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	dpi = &obj->dpi;
	ret = bnxt_qplib_alloc_uc_dpi(&rdev->qplib_res, dpi);
	if (ret)
		goto free_mem;

	obj->entry = bnxt_re_mmap_entry_insert(uctx, dpi->umdbr,
					       BNXT_RE_MMAP_UC_DB,
					       &mmap_offset);
	if (!obj->entry) {
		ret = -ENOMEM;
		goto free_dpi;
	}

	obj->rdev = rdev;
	uobj->object = obj;
	uverbs_finalize_uobj_create(attrs, BNXT_RE_DV_ALLOC_DBR_HANDLE);

	dbr.umdbr = dpi->umdbr;
	dbr.dpi = dpi->dpi;
	ret = uverbs_copy_to_struct_or_zero(attrs, BNXT_RE_DV_ALLOC_DBR_ATTR,
					    &dbr, sizeof(dbr));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs, BNXT_RE_DV_ALLOC_DBR_OFFSET,
			     &mmap_offset, sizeof(mmap_offset));
	if (ret)
		return ret;
	return 0;
free_dpi:
	bnxt_qplib_free_uc_dpi(&rdev->qplib_res, dpi);
free_mem:
	kfree(obj);
	return ret;
}

static int bnxt_re_dv_dbr_cleanup(struct ib_uobject *uobject,
				  enum rdma_remove_reason why,
				  struct uverbs_attr_bundle *attrs)
{
	struct bnxt_re_dbr_obj *obj = uobject->object;
	struct bnxt_re_dev *rdev = obj->rdev;

	if (atomic_read(&obj->usecnt))
		return -EBUSY;

	rdma_user_mmap_entry_remove(&obj->entry->rdma_entry);
	bnxt_qplib_free_uc_dpi(&rdev->qplib_res, &obj->dpi);
	return 0;
}

static int UVERBS_HANDLER(BNXT_RE_METHOD_GET_DEFAULT_DBR)(struct uverbs_attr_bundle *attrs)
{
	struct bnxt_re_dv_db_region dpi = {};
	struct bnxt_re_ucontext *uctx;
	struct ib_ucontext *ib_uctx;
	int ret;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	dpi.umdbr = uctx->dpi.umdbr;
	dpi.dpi = uctx->dpi.dpi;

	ret = uverbs_copy_to_struct_or_zero(attrs, BNXT_RE_DV_DEFAULT_DBR_ATTR,
					    &dpi, sizeof(dpi));
	if (ret)
		return ret;

	return 0;
}

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_DBR_ALLOC,
			    UVERBS_ATTR_IDR(BNXT_RE_DV_ALLOC_DBR_HANDLE,
					    BNXT_RE_OBJECT_DBR,
					    UVERBS_ACCESS_NEW,
					    UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_DV_ALLOC_DBR_ATTR,
						UVERBS_ATTR_STRUCT(struct bnxt_re_dv_db_region,
								   umdbr),
								   UA_MANDATORY),
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_DV_ALLOC_DBR_OFFSET,
						UVERBS_ATTR_TYPE(u64),
						UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(BNXT_RE_METHOD_DBR_FREE,
				    UVERBS_ATTR_IDR(BNXT_RE_DV_FREE_DBR_HANDLE,
						    BNXT_RE_OBJECT_DBR,
						    UVERBS_ACCESS_DESTROY,
						    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(BNXT_RE_OBJECT_DBR,
			    UVERBS_TYPE_ALLOC_IDR(bnxt_re_dv_dbr_cleanup),
			    &UVERBS_METHOD(BNXT_RE_METHOD_DBR_ALLOC),
			    &UVERBS_METHOD(BNXT_RE_METHOD_DBR_FREE));

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_GET_DEFAULT_DBR,
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_DV_DEFAULT_DBR_ATTR,
						UVERBS_ATTR_STRUCT(struct bnxt_re_dv_db_region,
								   umdbr),
						UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(BNXT_RE_OBJECT_DEFAULT_DBR,
			      &UVERBS_METHOD(BNXT_RE_METHOD_GET_DEFAULT_DBR));

static int bnxt_re_dv_create_cq_resp(struct bnxt_re_dev *rdev,
				     struct bnxt_re_cq *cq,
				     struct bnxt_re_cq_resp *resp)
{
	struct bnxt_qplib_cq *qplcq = &cq->qplib_cq;

	resp->cqid = qplcq->id;
	resp->tail = qplcq->hwq.cons;
	resp->phase = qplcq->period;
	resp->comp_mask = BNXT_RE_CQ_DV_SUPPORT;
	return 0;
}

static struct ib_umem *bnxt_re_dv_umem_get(struct bnxt_re_dev *rdev,
					   struct ib_ucontext *ib_uctx,
					   int dmabuf_fd,
					   u64 addr, u64 size,
					   struct bnxt_qplib_sg_info *sg)
{
	int access = IB_ACCESS_LOCAL_WRITE;
	unsigned long page_size;
	struct ib_umem *umem;
	int umem_pgs, rc;

	if (dmabuf_fd) {
		struct ib_umem_dmabuf *umem_dmabuf;

		umem_dmabuf = ib_umem_dmabuf_get_pinned(&rdev->ibdev, addr, size,
							dmabuf_fd, access);
		if (IS_ERR(umem_dmabuf)) {
			rc = PTR_ERR(umem_dmabuf);
			goto umem_err;
		}
		umem = &umem_dmabuf->umem;
	} else {
		umem = ib_umem_get(&rdev->ibdev, addr, size, access);
		if (IS_ERR(umem)) {
			rc = PTR_ERR(umem);
			goto umem_err;
		}
	}

	page_size = ib_umem_find_best_pgsz(umem, SZ_4K, addr);
	if (!page_size) {
		rc = -EINVAL;
		goto umem_rel;
	}
	umem_pgs = ib_umem_num_dma_blocks(umem, SZ_4K);
	if (!umem_pgs) {
		rc = -EINVAL;
		goto umem_rel;
	}
	sg->npages = umem_pgs;
	sg->pgsize = SZ_4K;
	sg->pgshft = __builtin_ctz(SZ_4K);
	sg->umem = umem;
	return umem;

umem_rel:
	ib_umem_release(umem);
umem_err:
	return ERR_PTR(rc);
}

static int bnxt_re_dv_create_qplib_cq(struct bnxt_re_dev *rdev,
				      struct bnxt_re_ucontext *re_uctx,
				      struct bnxt_re_cq *cq,
				      struct bnxt_re_cq_req *req)
{
	struct bnxt_qplib_dev_attr *dev_attr = rdev->dev_attr;
	struct bnxt_qplib_cq *qplcq;
	struct ib_umem *umem;
	u32 cqe = req->ncqe;
	u32 max_active_cqs;
	int rc = -EINVAL;

	if (!atomic_add_unless(&rdev->stats.res.cq_count, 1, dev_attr->max_cq)) {
		ibdev_dbg(&rdev->ibdev, "Create CQ failed - max exceeded(CQs)");
		return rc;
	}

	/* Validate CQ fields */
	if (cqe < 1 || cqe > dev_attr->max_cq_wqes) {
		ibdev_dbg(&rdev->ibdev, "Create CQ failed - max exceeded(CQ_WQs)");
		goto fail_dec;
	}

	qplcq = &cq->qplib_cq;
	qplcq->cq_handle = (u64)qplcq;
	umem = bnxt_re_dv_umem_get(rdev, &re_uctx->ib_uctx, req->dmabuf_fd,
				   req->cq_va, cqe * sizeof(struct cq_base),
				   &qplcq->sg_info);
	if (IS_ERR(umem)) {
		rc = PTR_ERR(umem);
		ibdev_dbg(&rdev->ibdev,
			  "bnxt_re_dv_umem_get() failed, rc: %d\n", rc);
		goto fail_dec;
	}
	cq->umem = umem;
	qplcq->dpi = &re_uctx->dpi;
	qplcq->max_wqe = cqe;
	qplcq->nq = bnxt_re_get_nq(rdev);
	qplcq->cnq_hw_ring_id = qplcq->nq->ring_id;
	qplcq->coalescing = &rdev->cq_coalescing;
	rc = bnxt_qplib_create_cq(&rdev->qplib_res, qplcq);
	if (rc) {
		ibdev_err(&rdev->ibdev, "Failed to create HW CQ");
		goto fail_qpl;
	}

	cq->ib_cq.cqe = cqe;
	cq->cq_period = qplcq->period;

	max_active_cqs = atomic_read(&rdev->stats.res.cq_count);
	if (max_active_cqs > rdev->stats.res.cq_watermark)
		rdev->stats.res.cq_watermark = max_active_cqs;
	spin_lock_init(&cq->cq_lock);

	return 0;

fail_qpl:
	ib_umem_release(cq->umem);
fail_dec:
	atomic_dec(&rdev->stats.res.cq_count);
	return rc;
}

static void bnxt_re_dv_free_qplib_cq(struct bnxt_re_dev *rdev,
				     struct bnxt_re_cq *re_cq)
{
	bnxt_qplib_destroy_cq(&rdev->qplib_res, &re_cq->qplib_cq);
	bnxt_re_put_nq(rdev, re_cq->qplib_cq.nq);
	ib_umem_release(re_cq->umem);
}

int bnxt_re_dv_create_cq(struct bnxt_re_dev *rdev, struct ib_udata *udata,
			 struct bnxt_re_cq *re_cq, struct bnxt_re_cq_req *req)
{
	struct bnxt_re_ucontext *re_uctx =
		rdma_udata_to_drv_context(udata, struct bnxt_re_ucontext, ib_uctx);
	struct bnxt_re_cq_resp resp = {};
	int ret;

	ret = bnxt_re_dv_create_qplib_cq(rdev, re_uctx, re_cq, req);
	if (ret)
		return ret;

	ret = bnxt_re_dv_create_cq_resp(rdev, re_cq, &resp);
	if (ret)
		goto fail_resp;

	ret = ib_copy_to_udata(udata, &resp, min(sizeof(resp), udata->outlen));
	if (ret)
		goto fail_resp;

	re_cq->is_dv_cq = true;
	atomic_inc(&rdev->dv_cq_count);
	return 0;

fail_resp:
	bnxt_re_dv_free_qplib_cq(rdev, re_cq);
	return ret;
};

static int bnxt_re_dv_init_qp_attr(struct bnxt_re_qp *qp,
				   struct bnxt_re_ucontext *cntx,
				   struct ib_qp_init_attr *init_attr,
				   struct bnxt_re_qp_req *req,
				   struct bnxt_re_dbr_obj *dbr_obj)
{
	struct bnxt_qplib_dev_attr *dev_attr;
	struct bnxt_qplib_qp *qplqp;
	struct bnxt_re_cq *send_cq;
	struct bnxt_re_cq *recv_cq;
	struct bnxt_re_dev *rdev;
	struct bnxt_qplib_q *rq;
	struct bnxt_qplib_q *sq;
	u32 slot_size;
	int qptype;

	rdev = qp->rdev;
	qplqp = &qp->qplib_qp;
	dev_attr = rdev->dev_attr;

	/* Setup misc params */
	qplqp->is_user = true;
	qplqp->pd_id = req->pd_id;
	qplqp->qp_handle = (u64)qplqp;
	qplqp->sig_type = false;
	qptype = __from_ib_qp_type(init_attr->qp_type);
	if (qptype < 0)
		return qptype;
	qplqp->type = (u8)qptype;
	qplqp->wqe_mode = rdev->chip_ctx->modes.wqe_mode;
	ether_addr_copy(qplqp->smac, rdev->netdev->dev_addr);
	qplqp->dev_cap_flags = dev_attr->dev_cap_flags;
	qplqp->cctx = rdev->chip_ctx;

	if (init_attr->qp_type == IB_QPT_RC) {
		qplqp->max_rd_atomic = dev_attr->max_qp_rd_atom;
		qplqp->max_dest_rd_atomic = dev_attr->max_qp_init_rd_atom;
	}
	qplqp->mtu = ib_mtu_enum_to_int(iboe_get_mtu(rdev->netdev->mtu));
	if (dbr_obj)
		qplqp->dpi = &dbr_obj->dpi;
	else
		qplqp->dpi = &cntx->dpi;

	/* Setup CQs */
	if (!init_attr->send_cq)
		return -EINVAL;
	send_cq = container_of(init_attr->send_cq, struct bnxt_re_cq, ib_cq);
	qplqp->scq = &send_cq->qplib_cq;
	qp->scq = send_cq;

	if (!init_attr->recv_cq)
		return -EINVAL;
	recv_cq = container_of(init_attr->recv_cq, struct bnxt_re_cq, ib_cq);
	qplqp->rcq = &recv_cq->qplib_cq;
	qp->rcq = recv_cq;

	if (!init_attr->srq) {
		/* Setup RQ */
		slot_size = bnxt_qplib_get_stride();
		rq = &qplqp->rq;
		rq->max_sge = init_attr->cap.max_recv_sge;
		rq->wqe_size = req->rq_wqe_sz;
		rq->max_wqe = (req->rq_slots * slot_size) /
				req->rq_wqe_sz;
		rq->max_sw_wqe = rq->max_wqe;
		rq->q_full_delta = 0;
		rq->sg_info.pgsize = PAGE_SIZE;
		rq->sg_info.pgshft = PAGE_SHIFT;
	}

	/* Setup SQ */
	sq = &qplqp->sq;
	sq->max_sge = init_attr->cap.max_send_sge;
	sq->wqe_size = req->sq_wqe_sz;
	sq->max_wqe = req->sq_slots; /* SQ in var-wqe mode */
	sq->max_sw_wqe = sq->max_wqe;
	sq->q_full_delta = 0;
	sq->sg_info.pgsize = PAGE_SIZE;
	sq->sg_info.pgshft = PAGE_SHIFT;

	return 0;
}

static int bnxt_re_dv_init_user_qp(struct bnxt_re_dev *rdev,
				   struct bnxt_re_ucontext *cntx,
				   struct bnxt_re_qp *qp,
				   struct ib_qp_init_attr *init_attr,
				   struct bnxt_re_qp_req *req)
{
	struct bnxt_qplib_sg_info *sginfo;
	struct bnxt_qplib_qp *qplib_qp;
	struct ib_umem *umem;
	int rc = -EINVAL;

	qplib_qp = &qp->qplib_qp;
	qplib_qp->qp_handle = req->qp_handle;
	sginfo = &qplib_qp->sq.sg_info;

	/* SQ */
	umem = bnxt_re_dv_umem_get(rdev, &cntx->ib_uctx, req->sq_dmabuf_fd,
				   req->qpsva, req->sq_len, sginfo);
	if (IS_ERR(umem)) {
		rc = PTR_ERR(umem);
		ibdev_dbg(&rdev->ibdev,
			  "bnxt_re_dv_umem_get() failed, rc: %d\n", rc);
		return rc;
	}
	qp->sumem = umem;

	/* SRQ */
	if (init_attr->srq) {
		struct bnxt_re_srq *srq;

		srq = container_of(init_attr->srq, struct bnxt_re_srq, ib_srq);
		qplib_qp->srq = &srq->qplib_srq;
		goto done;
	}

	/* RQ */
	sginfo = &qplib_qp->rq.sg_info;
	umem = bnxt_re_dv_umem_get(rdev, &cntx->ib_uctx, req->rq_dmabuf_fd,
				   req->qprva, req->rq_len, sginfo);
	if (IS_ERR(umem)) {
		rc = PTR_ERR(umem);
		ibdev_dbg(&rdev->ibdev,
			  "bnxt_re_dv_umem_get() failed, rc: %d\n", rc);
		goto rqfail;
	}
	qp->rumem = umem;
done:
	qplib_qp->is_user = true;
	return 0;
rqfail:
	ib_umem_release(qp->sumem);
	qplib_qp->sq.sg_info.umem = NULL;
	return rc;
}

static int
bnxt_re_dv_qp_init_msn(struct bnxt_re_dev *rdev, struct bnxt_re_qp *qp,
		       struct bnxt_re_qp_req *req)
{
	struct bnxt_qplib_dev_attr *dev_attr = rdev->dev_attr;
	struct bnxt_qplib_qp *qplib_qp = &qp->qplib_qp;

	if (req->sq_npsn > dev_attr->max_qp_wqes ||
	    req->sq_psn_sz > sizeof(struct sq_psn_search_ext))
		return -EINVAL;

	qplib_qp->is_host_msn_tbl = true;
	qplib_qp->msn = 0;
	qplib_qp->psn_sz = req->sq_psn_sz;
	qplib_qp->msn_tbl_sz = req->sq_psn_sz * req->sq_npsn;
	return 0;
}

static void bnxt_re_dv_init_qp(struct bnxt_re_dev *rdev,
			       struct bnxt_re_qp *qp)
{
	u32 active_qps, tmp_qps;

	spin_lock_init(&qp->sq_lock);
	spin_lock_init(&qp->rq_lock);
	INIT_LIST_HEAD(&qp->list);
	mutex_lock(&rdev->qp_lock);
	list_add_tail(&qp->list, &rdev->qp_list);
	mutex_unlock(&rdev->qp_lock);
	atomic_inc(&rdev->stats.res.qp_count);
	active_qps = atomic_read(&rdev->stats.res.qp_count);
	if (active_qps > rdev->stats.res.qp_watermark)
		rdev->stats.res.qp_watermark = active_qps;

	/* Get the counters for RC QPs */
	tmp_qps = atomic_inc_return(&rdev->stats.res.rc_qp_count);
	if (tmp_qps > rdev->stats.res.rc_qp_watermark)
		rdev->stats.res.rc_qp_watermark = tmp_qps;
}

int bnxt_re_dv_create_qp(struct bnxt_re_dev *rdev, struct ib_udata *udata,
			 struct ib_qp_init_attr *init_attr,
			 struct bnxt_re_qp *re_qp, struct bnxt_re_qp_req *req)
{
	struct bnxt_re_dbr_obj *dbr_obj = NULL;
	struct bnxt_re_cq *send_cq = NULL;
	struct bnxt_re_cq *recv_cq = NULL;
	struct bnxt_re_qp_resp resp = {};
	struct uverbs_attr_bundle *attrs;
	struct bnxt_re_ucontext *uctx;
	int ret;

	uctx = rdma_udata_to_drv_context(udata, struct bnxt_re_ucontext, ib_uctx);
	if (init_attr->send_cq) {
		send_cq = container_of(init_attr->send_cq, struct bnxt_re_cq, ib_cq);
		re_qp->scq = send_cq;
	}

	if (init_attr->recv_cq) {
		recv_cq = container_of(init_attr->recv_cq, struct bnxt_re_cq, ib_cq);
		re_qp->rcq = recv_cq;
	}

	attrs = rdma_udata_to_uverbs_attr_bundle(udata);
	if (!attrs)
		return -EINVAL;

	if (uverbs_attr_is_valid(attrs, BNXT_RE_CREATE_QP_ATTR_DBR_HANDLE)) {
		dbr_obj = uverbs_attr_get_obj(attrs, BNXT_RE_CREATE_QP_ATTR_DBR_HANDLE);
		if (IS_ERR(dbr_obj))
			return PTR_ERR(dbr_obj);
		atomic_inc(&dbr_obj->usecnt);
		re_qp->dbr_obj = dbr_obj;
	}

	re_qp->rdev = rdev;
	ret = bnxt_re_dv_init_qp_attr(re_qp, uctx, init_attr, req, dbr_obj);
	if (ret)
		goto dbr_rel;

	ret = bnxt_re_dv_init_user_qp(rdev, uctx, re_qp, init_attr, req);
	if (ret)
		goto dbr_rel;

	ret = bnxt_re_dv_qp_init_msn(rdev, re_qp, req);
	if (ret)
		goto free_umem;

	ret = bnxt_re_setup_qp_hwqs(re_qp, true);
	if (ret)
		goto free_umem;

	ret = bnxt_qplib_create_qp(&rdev->qplib_res, &re_qp->qplib_qp);
	if (ret) {
		ibdev_err(&rdev->ibdev, "Failed to create HW QP");
		goto free_hwq;
	}

	resp.qpid = re_qp->qplib_qp.id;
	resp.comp_mask = BNXT_RE_QP_DV_SUPPORT;
	resp.rsvd = 0;
	ret = ib_copy_to_udata(udata, &resp, sizeof(resp));
	if (ret)
		goto free_qplib;

	bnxt_re_dv_init_qp(rdev, re_qp);
	re_qp->is_dv_qp = true;
	atomic_inc(&rdev->dv_qp_count);
	return 0;

free_qplib:
	bnxt_qplib_destroy_qp(&rdev->qplib_res, &re_qp->qplib_qp);
free_hwq:
	bnxt_qplib_free_qp_res(&rdev->qplib_res, &re_qp->qplib_qp);
free_umem:
	bnxt_re_qp_free_umem(re_qp);
dbr_rel:
	if (dbr_obj)
		atomic_dec(&dbr_obj->usecnt);
	return ret;
}

int bnxt_re_dv_destroy_qp(struct bnxt_re_qp *qp)
{
	struct bnxt_re_dev *rdev = qp->rdev;
	struct bnxt_qplib_qp *qplib_qp = &qp->qplib_qp;
	struct bnxt_qplib_nq *scq_nq = NULL;
	struct bnxt_qplib_nq *rcq_nq = NULL;
	int rc;

	mutex_lock(&rdev->qp_lock);
	list_del(&qp->list);
	atomic_dec(&rdev->stats.res.qp_count);
	if (qp->qplib_qp.type == CMDQ_CREATE_QP_TYPE_RC)
		atomic_dec(&rdev->stats.res.rc_qp_count);
	mutex_unlock(&rdev->qp_lock);

	rc = bnxt_qplib_destroy_qp(&rdev->qplib_res, qplib_qp);
	if (rc)
		ibdev_err_ratelimited(&rdev->ibdev,
				      "id = %d failed rc = %d",
				      qplib_qp->id, rc);

	bnxt_qplib_free_qp_res(&rdev->qplib_res, qplib_qp);
	bnxt_re_qp_free_umem(qp);

	/* Flush all the entries of notification queue associated with
	 * given qp.
	 */
	scq_nq = qplib_qp->scq->nq;
	rcq_nq = qplib_qp->rcq->nq;
	bnxt_re_synchronize_nq(scq_nq);
	if (scq_nq != rcq_nq)
		bnxt_re_synchronize_nq(rcq_nq);

	atomic_dec(&rdev->dv_qp_count);
	if (qp->dbr_obj)
		atomic_dec(&qp->dbr_obj->usecnt);
	return 0;
}

ADD_UVERBS_ATTRIBUTES_SIMPLE(
	bnxt_re_qp_create,
	UVERBS_OBJECT_QP,
	UVERBS_METHOD_QP_CREATE,
	UVERBS_ATTR_IDR(BNXT_RE_CREATE_QP_ATTR_DBR_HANDLE,
			BNXT_RE_OBJECT_DBR,
			UVERBS_ACCESS_READ,
			UA_OPTIONAL));

const struct uapi_definition bnxt_re_create_qp_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE(UVERBS_OBJECT_QP, &bnxt_re_qp_create),
	{},
};

const struct uapi_definition bnxt_re_uapi_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_ALLOC_PAGE),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_NOTIFY_DRV),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_GET_TOGGLE_MEM),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_DBR),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_DEFAULT_DBR),
	UAPI_DEF_CHAIN(bnxt_re_create_qp_defs),
	{}
};
