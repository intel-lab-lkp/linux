// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/*
 * Copyright Amazon.com, Inc. or its affiliates. All rights reserved.
 */

#include <rdma/uverbs_std_types.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_umem_dmabuf.h>
#include "rdma_core.h"
#include "uverbs.h"

static int uverbs_free_comp_cntr(struct ib_uobject *uobject,
				 enum rdma_remove_reason why,
				 struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uobject->object;
	int ret;

	ret = cc->device->ops.destroy_comp_cntr(cc);
	if (ret)
		return ret;

	ib_umem_release(cc->comp_umem);
	ib_umem_release(cc->err_umem);
	kfree(cc);
	return 0;
}

static int comp_cntr_get_umem(struct ib_device *ib_dev,
			      struct uverbs_attr_bundle *attrs,
			      int va_attr, int fd_attr, int offset_attr,
			      struct ib_umem **umem_out)
{
	struct ib_umem_dmabuf *umem_dmabuf;
	u64 buffer_offset;
	u64 buffer_va;
	int buffer_fd;
	int ret;

	*umem_out = NULL;

	if (uverbs_attr_is_valid(attrs, va_attr)) {
		if (uverbs_attr_is_valid(attrs, fd_attr) ||
		    uverbs_attr_is_valid(attrs, offset_attr))
			return -EINVAL;

		ret = uverbs_copy_from(&buffer_va, attrs, va_attr);
		if (ret)
			return ret;

		*umem_out = ib_umem_get(ib_dev, buffer_va, sizeof(u64),
					IB_ACCESS_LOCAL_WRITE);
		if (IS_ERR(*umem_out)) {
			ret = PTR_ERR(*umem_out);
			*umem_out = NULL;
			return ret;
		}
	} else if (uverbs_attr_is_valid(attrs, fd_attr)) {
		if (uverbs_attr_is_valid(attrs, va_attr))
			return -EINVAL;

		ret = uverbs_get_raw_fd(&buffer_fd, attrs, fd_attr);
		if (ret)
			return ret;

		ret = uverbs_copy_from(&buffer_offset, attrs, offset_attr);
		if (ret)
			return ret;

		umem_dmabuf = ib_umem_dmabuf_get_pinned(ib_dev, buffer_offset,
							sizeof(u64), buffer_fd,
							IB_ACCESS_LOCAL_WRITE);
		if (IS_ERR(umem_dmabuf))
			return PTR_ERR(umem_dmabuf);

		*umem_out = &umem_dmabuf->umem;
	}

	return 0;
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_CREATE)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_uobject *uobj = uverbs_attr_get_uobject(
		attrs, UVERBS_ATTR_CREATE_COMP_CNTR_HANDLE);
	struct ib_device *ib_dev = attrs->context->device;
	struct ib_comp_cntr *cc;
	int ret;

	if (!ib_dev->ops.create_comp_cntr ||
	    !ib_dev->ops.destroy_comp_cntr ||
	    !ib_dev->ops.qp_attach_comp_cntr)
		return -EOPNOTSUPP;

	cc = rdma_zalloc_drv_obj(ib_dev, ib_comp_cntr);
	if (!cc)
		return -ENOMEM;

	cc->device = ib_dev;
	cc->uobject = uobj;

	ret = comp_cntr_get_umem(ib_dev, attrs,
				 UVERBS_ATTR_CREATE_COMP_CNTR_BUFFER_VA,
				 UVERBS_ATTR_CREATE_COMP_CNTR_BUFFER_FD,
				 UVERBS_ATTR_CREATE_COMP_CNTR_BUFFER_OFFSET,
				 &cc->comp_umem);
	if (ret)
		goto err_free;

	ret = comp_cntr_get_umem(ib_dev, attrs,
				 UVERBS_ATTR_CREATE_COMP_CNTR_ERR_BUFFER_VA,
				 UVERBS_ATTR_CREATE_COMP_CNTR_ERR_BUFFER_FD,
				 UVERBS_ATTR_CREATE_COMP_CNTR_ERR_BUFFER_OFFSET,
				 &cc->err_umem);
	if (ret)
		goto err_comp_umem;

	ret = ib_dev->ops.create_comp_cntr(cc, attrs);
	if (ret)
		goto err_err_umem;

	uobj->object = cc;
	uverbs_finalize_uobj_create(attrs, UVERBS_ATTR_CREATE_COMP_CNTR_HANDLE);

	ret = uverbs_copy_to(attrs,
			     UVERBS_ATTR_CREATE_COMP_CNTR_RESP_COUNT_MAX_VALUE,
			     &cc->comp_count_max_value,
			     sizeof(cc->comp_count_max_value));
	if (ret)
		return ret;

	ret = uverbs_copy_to(attrs,
			     UVERBS_ATTR_CREATE_COMP_CNTR_RESP_ERR_COUNT_MAX_VALUE,
			     &cc->err_count_max_value,
			     sizeof(cc->err_count_max_value));
	return ret;

err_err_umem:
	ib_umem_release(cc->err_umem);
err_comp_umem:
	ib_umem_release(cc->comp_umem);
err_free:
	kfree(cc);
	return ret;
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_SET)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_SET_COMP_CNTR_HANDLE);
	u64 value;
	int ret;

	if (!cc->device->ops.set_comp_cntr)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&value, attrs, UVERBS_ATTR_SET_COMP_CNTR_VALUE);
	if (ret)
		return ret;

	return cc->device->ops.set_comp_cntr(cc, value);
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_SET_ERR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_SET_ERR_COMP_CNTR_HANDLE);
	u64 value;
	int ret;

	if (!cc->device->ops.set_err_comp_cntr)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&value, attrs,
			       UVERBS_ATTR_SET_ERR_COMP_CNTR_VALUE);
	if (ret)
		return ret;

	return cc->device->ops.set_err_comp_cntr(cc, value);
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_INC)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_INC_COMP_CNTR_HANDLE);
	u64 amount;
	int ret;

	if (!cc->device->ops.inc_comp_cntr)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&amount, attrs, UVERBS_ATTR_INC_COMP_CNTR_VALUE);
	if (ret)
		return ret;

	return cc->device->ops.inc_comp_cntr(cc, amount);
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_INC_ERR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_INC_ERR_COMP_CNTR_HANDLE);
	u64 amount;
	int ret;

	if (!cc->device->ops.inc_err_comp_cntr)
		return -EOPNOTSUPP;

	ret = uverbs_copy_from(&amount, attrs,
			       UVERBS_ATTR_INC_ERR_COMP_CNTR_VALUE);
	if (ret)
		return ret;

	return cc->device->ops.inc_err_comp_cntr(cc, amount);
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_READ)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_READ_COMP_CNTR_HANDLE);
	u64 value;
	int ret;

	if (!cc->device->ops.read_comp_cntr)
		return -EOPNOTSUPP;

	ret = cc->device->ops.read_comp_cntr(cc, &value);
	if (ret)
		return ret;

	return uverbs_copy_to(attrs, UVERBS_ATTR_READ_COMP_CNTR_RESP_VALUE,
			      &value, sizeof(value));
}

static int UVERBS_HANDLER(UVERBS_METHOD_COMP_CNTR_READ_ERR)(
	struct uverbs_attr_bundle *attrs)
{
	struct ib_comp_cntr *cc = uverbs_attr_get_obj(
		attrs, UVERBS_ATTR_READ_ERR_COMP_CNTR_HANDLE);
	u64 value;
	int ret;

	if (!cc->device->ops.read_err_comp_cntr)
		return -EOPNOTSUPP;

	ret = cc->device->ops.read_err_comp_cntr(cc, &value);
	if (ret)
		return ret;

	return uverbs_copy_to(attrs, UVERBS_ATTR_READ_ERR_COMP_CNTR_RESP_VALUE,
			      &value, sizeof(value));
}

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_CREATE,
	UVERBS_ATTR_IDR(UVERBS_ATTR_CREATE_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_NEW,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_CREATE_COMP_CNTR_BUFFER_VA,
			   UVERBS_ATTR_TYPE(u64),
			   UA_OPTIONAL),
	UVERBS_ATTR_RAW_FD(UVERBS_ATTR_CREATE_COMP_CNTR_BUFFER_FD,
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_CREATE_COMP_CNTR_BUFFER_OFFSET,
			   UVERBS_ATTR_TYPE(u64),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_CREATE_COMP_CNTR_ERR_BUFFER_VA,
			   UVERBS_ATTR_TYPE(u64),
			   UA_OPTIONAL),
	UVERBS_ATTR_RAW_FD(UVERBS_ATTR_CREATE_COMP_CNTR_ERR_BUFFER_FD,
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_CREATE_COMP_CNTR_ERR_BUFFER_OFFSET,
			   UVERBS_ATTR_TYPE(u64),
			   UA_OPTIONAL),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_CREATE_COMP_CNTR_RESP_COUNT_MAX_VALUE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_CREATE_COMP_CNTR_RESP_ERR_COUNT_MAX_VALUE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY),
	UVERBS_ATTR_UHW());

DECLARE_UVERBS_NAMED_METHOD_DESTROY(
	UVERBS_METHOD_COMP_CNTR_DESTROY,
	UVERBS_ATTR_IDR(UVERBS_ATTR_DESTROY_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_DESTROY,
			UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_SET,
	UVERBS_ATTR_IDR(UVERBS_ATTR_SET_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_WRITE,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_SET_COMP_CNTR_VALUE,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_SET_ERR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_SET_ERR_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_WRITE,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_SET_ERR_COMP_CNTR_VALUE,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_INC,
	UVERBS_ATTR_IDR(UVERBS_ATTR_INC_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_WRITE,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_INC_COMP_CNTR_VALUE,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_INC_ERR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_INC_ERR_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_WRITE,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(UVERBS_ATTR_INC_ERR_COMP_CNTR_VALUE,
			   UVERBS_ATTR_TYPE(u64),
			   UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_READ,
	UVERBS_ATTR_IDR(UVERBS_ATTR_READ_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_READ_COMP_CNTR_RESP_VALUE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	UVERBS_METHOD_COMP_CNTR_READ_ERR,
	UVERBS_ATTR_IDR(UVERBS_ATTR_READ_ERR_COMP_CNTR_HANDLE,
			UVERBS_OBJECT_COMP_CNTR,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(UVERBS_ATTR_READ_ERR_COMP_CNTR_RESP_VALUE,
			    UVERBS_ATTR_TYPE(u64),
			    UA_MANDATORY));

DECLARE_UVERBS_NAMED_OBJECT(
	UVERBS_OBJECT_COMP_CNTR,
	UVERBS_TYPE_ALLOC_IDR(uverbs_free_comp_cntr),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_CREATE),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_DESTROY),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_SET),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_SET_ERR),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_INC),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_INC_ERR),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_READ),
	&UVERBS_METHOD(UVERBS_METHOD_COMP_CNTR_READ_ERR));

const struct uapi_definition uverbs_def_obj_comp_cntr[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(UVERBS_OBJECT_COMP_CNTR,
				      UAPI_DEF_OBJ_NEEDS_FN(destroy_comp_cntr)),
	{}
};
