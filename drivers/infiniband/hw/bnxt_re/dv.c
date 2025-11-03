/*
 * Broadcom NetXtreme-E RoCE driver.
 *
 * Copyright (c) 2025, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Inc. and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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

struct bnxt_re_dv_umem {
	struct bnxt_re_dev *rdev;
	struct ib_umem *umem;
	__u64 addr;
	size_t size;
	__u32 access;
	int dmabuf_fd;
};

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

	uctx = container_of(ib_uverbs_get_ucontext(attrs), struct bnxt_re_ucontext, ib_uctx);
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
	struct bnxt_re_dev *rdev;
	u64 mmap_offset;
	u32 length;
	u32 dpi;
	u64 addr;
	int err;

	uctx = container_of(ib_uverbs_get_ucontext(attrs), struct bnxt_re_ucontext, ib_uctx);
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

static int bnxt_re_dv_validate_umem_attr(struct bnxt_re_dev *rdev,
					 struct uverbs_attr_bundle *attrs,
					 struct bnxt_re_dv_umem *obj)
{
	int dmabuf_fd = 0;
	u32 access_flags;
	size_t size;
	u64 addr;
	int err;

	err = uverbs_get_flags32(&access_flags, attrs,
				 BNXT_RE_UMEM_OBJ_REG_ACCESS,
				 IB_ACCESS_LOCAL_WRITE |
				 IB_ACCESS_REMOTE_WRITE |
				 IB_ACCESS_REMOTE_READ);
	if (err)
		return err;

	err = ib_check_mr_access(&rdev->ibdev, access_flags);
	if (err)
		return err;

	if (uverbs_copy_from(&addr, attrs, BNXT_RE_UMEM_OBJ_REG_ADDR) ||
	    uverbs_copy_from(&size, attrs, BNXT_RE_UMEM_OBJ_REG_LEN))
		return -EFAULT;
	if (uverbs_attr_is_valid(attrs, BNXT_RE_UMEM_OBJ_REG_DMABUF_FD)) {
		if (uverbs_get_raw_fd(&dmabuf_fd, attrs,
				      BNXT_RE_UMEM_OBJ_REG_DMABUF_FD))
			return -EFAULT;
	}
	obj->addr = addr;
	obj->size = size;
	obj->access = access_flags;
	obj->dmabuf_fd = dmabuf_fd;

	return 0;
}

static int bnxt_re_dv_umem_cleanup(struct ib_uobject *uobject,
				   enum rdma_remove_reason why,
				   struct uverbs_attr_bundle *attrs)
{
	kfree(uobject->object);
	return 0;
}

static int UVERBS_HANDLER(BNXT_RE_METHOD_UMEM_REG)(struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj =
		uverbs_attr_get_uobject(attrs, BNXT_RE_UMEM_OBJ_REG_HANDLE);
	struct bnxt_re_ucontext *uctx;
	struct ib_ucontext *ib_uctx;
	struct bnxt_re_dv_umem *obj;
	struct bnxt_re_dev *rdev;
	int err;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	rdev = uctx->rdev;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	obj->rdev = rdev;
	err = bnxt_re_dv_validate_umem_attr(rdev, attrs, obj);
	if (err)
		goto free_mem;

	obj->umem = NULL;
	uobj->object = obj;
	uverbs_finalize_uobj_create(attrs, BNXT_RE_UMEM_OBJ_REG_HANDLE);

	return 0;
free_mem:
	kfree(obj);
	return err;
}

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_UMEM_REG,
			    UVERBS_ATTR_IDR(BNXT_RE_UMEM_OBJ_REG_HANDLE,
					    BNXT_RE_OBJECT_UMEM,
					    UVERBS_ACCESS_NEW,
					    UA_MANDATORY),
			    UVERBS_ATTR_PTR_IN(BNXT_RE_UMEM_OBJ_REG_ADDR,
					       UVERBS_ATTR_TYPE(u64),
					       UA_MANDATORY),
			    UVERBS_ATTR_PTR_IN(BNXT_RE_UMEM_OBJ_REG_LEN,
					       UVERBS_ATTR_TYPE(u64),
					       UA_MANDATORY),
			    UVERBS_ATTR_FLAGS_IN(BNXT_RE_UMEM_OBJ_REG_ACCESS,
						 enum ib_access_flags),
			    UVERBS_ATTR_RAW_FD(BNXT_RE_UMEM_OBJ_REG_DMABUF_FD,
					       UA_OPTIONAL),
			    UVERBS_ATTR_CONST_IN(BNXT_RE_UMEM_OBJ_REG_PGSZ_BITMAP,
						 u64));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(BNXT_RE_METHOD_UMEM_DEREG,
				    UVERBS_ATTR_IDR(BNXT_RE_UMEM_OBJ_DEREG_HANDLE,
						    BNXT_RE_OBJECT_UMEM,
						    UVERBS_ACCESS_DESTROY,
						    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(BNXT_RE_OBJECT_UMEM,
			    UVERBS_TYPE_ALLOC_IDR(bnxt_re_dv_umem_cleanup),
			    &UVERBS_METHOD(BNXT_RE_METHOD_UMEM_REG),
			    &UVERBS_METHOD(BNXT_RE_METHOD_UMEM_DEREG));

static int UVERBS_HANDLER(BNXT_RE_METHOD_DBR_ALLOC)(struct uverbs_attr_bundle *attrs)
{
	struct bnxt_re_dv_db_region dbr = {};
	struct bnxt_re_alloc_dbr_obj *obj;
	struct bnxt_re_ucontext *uctx;
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

	obj->entry = bnxt_re_mmap_entry_insert(uctx, 0, BNXT_RE_MMAP_UC_DB,
					       &mmap_offset);
	if (!obj->entry) {
		ret = -ENOMEM;
		goto free_dpi;
	}

	obj->rdev = rdev;
	dbr.umdbr = dpi->umdbr;
	dbr.dpi = dpi->dpi;

	ret = uverbs_copy_to_struct_or_zero(attrs, BNXT_RE_DV_ALLOC_DBR_ATTR,
					    &dbr, sizeof(dbr));
	if (ret)
		goto free_entry;

	ret = uverbs_copy_to(attrs, BNXT_RE_DV_ALLOC_DBR_OFFSET,
			     &mmap_offset, sizeof(mmap_offset));
	if (ret)
		goto free_entry;

	uobj->object = obj;
	uverbs_finalize_uobj_create(attrs, BNXT_RE_DV_ALLOC_DBR_HANDLE);
	return 0;
free_entry:
	rdma_user_mmap_entry_remove(&obj->entry->rdma_entry);
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
	struct bnxt_re_alloc_dbr_obj *obj = uobject->object;
	struct bnxt_re_dev *rdev = obj->rdev;

	rdma_user_mmap_entry_remove(&obj->entry->rdma_entry);
	bnxt_qplib_free_uc_dpi(&rdev->qplib_res, &obj->dpi);
	kfree(obj);
	return 0;
}

static int UVERBS_HANDLER(BNXT_RE_METHOD_DBR_QUERY)(struct uverbs_attr_bundle *attrs)
{
	struct bnxt_re_dv_db_region dpi = {};
	struct bnxt_re_ucontext *uctx;
	struct ib_ucontext *ib_uctx;
	struct bnxt_re_dev *rdev;
	int ret;

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);

	uctx = container_of(ib_uctx, struct bnxt_re_ucontext, ib_uctx);
	rdev = uctx->rdev;

	dpi.umdbr = uctx->dpi.umdbr;
	dpi.dpi = uctx->dpi.dpi;

	ret = uverbs_copy_to_struct_or_zero(attrs, BNXT_RE_DV_QUERY_DBR_ATTR,
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
								   dbr),
								   UA_MANDATORY),
			    UVERBS_ATTR_PTR_IN(BNXT_RE_DV_ALLOC_DBR_OFFSET,
					       UVERBS_ATTR_TYPE(u64),
					       UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(BNXT_RE_METHOD_DBR_QUERY,
			    UVERBS_ATTR_PTR_OUT(BNXT_RE_DV_QUERY_DBR_ATTR,
						UVERBS_ATTR_STRUCT(struct bnxt_re_dv_db_region,
								   dbr),
						UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD_DESTROY(BNXT_RE_METHOD_DBR_FREE,
				    UVERBS_ATTR_IDR(BNXT_RE_DV_FREE_DBR_HANDLE,
						    BNXT_RE_OBJECT_DBR,
						    UVERBS_ACCESS_DESTROY,
						    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(BNXT_RE_OBJECT_DBR,
			    UVERBS_TYPE_ALLOC_IDR(bnxt_re_dv_dbr_cleanup),
			    &UVERBS_METHOD(BNXT_RE_METHOD_DBR_ALLOC),
			    &UVERBS_METHOD(BNXT_RE_METHOD_DBR_FREE),
			    &UVERBS_METHOD(BNXT_RE_METHOD_DBR_QUERY));

const struct uapi_definition bnxt_re_uapi_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_ALLOC_PAGE),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_NOTIFY_DRV),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_GET_TOGGLE_MEM),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_DBR),
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(BNXT_RE_OBJECT_UMEM),
	{}
};
