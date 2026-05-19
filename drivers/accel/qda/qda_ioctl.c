// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_ioctl.h>
#include <drm/drm_print.h>
#include <drm/qda_accel.h>
#include "qda_drv.h"
#include "qda_fastrpc.h"
#include "qda_gem.h"
#include "qda_ioctl.h"
#include "qda_rpmsg.h"

/**
 * qda_ioctl_query() - Query DSP device information
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_query)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_query(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_query *args = data;
	struct qda_dev *qdev;

	qdev = qda_dev_from_drm(dev);

	strscpy(args->dsp_name, qdev->dsp_name, sizeof(args->dsp_name));

	return 0;
}

/**
 * qda_ioctl_gem_create() - Create a GEM buffer object
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_gem_create)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_create *args = data;
	struct drm_gem_object *gem_obj;
	struct qda_dev *qdev;

	if (args->pad)
		return -EINVAL;

	qdev = qda_dev_from_drm(dev);
	if (!qdev->iommu_mgr)
		return -ENODEV;

	gem_obj = qda_gem_create_object(dev, qdev->iommu_mgr, args->size, file_priv);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	return qda_gem_create_handle(file_priv, gem_obj, &args->handle);
}

/**
 * qda_ioctl_gem_mmap_offset() - Get the mmap offset for a GEM object
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_gem_mmap_offset)
 * @file_priv: DRM file private data
 *
 * Uses drm_gem_dumb_map_offset() which rejects imported dma-buf objects
 * (mmap of imported objects is not allowed).
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_gem_mmap_offset(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_mmap_offset *args = data;

	if (args->pad)
		return -EINVAL;

	return drm_gem_dumb_map_offset(file_priv, dev, args->handle, &args->offset);
}

static int fastrpc_context_get_id(struct fastrpc_invoke_context *ctx, struct qda_dev *qdev)
{
	int ret;
	u32 id;

	if (!qdev)
		return -EINVAL;

	ret = xa_alloc(&qdev->ctx_xa, &id, ctx, xa_limit_32b, GFP_KERNEL);
	if (ret)
		return ret;

	ctx->ctxid = id << 4;
	return 0;
}

static void fastrpc_context_put_id(struct fastrpc_invoke_context *ctx, struct qda_dev *qdev)
{
	if (qdev)
		xa_erase(&qdev->ctx_xa, ctx->ctxid >> 4);
}

static int fastrpc_invoke(int type, struct drm_device *dev, void *data,
			  struct drm_file *file_priv)
{
	struct qda_file_priv *qda_file_priv = file_priv->driver_priv;
	struct qda_dev *qdev = qda_file_priv->qda_dev;
	struct qda_msg msg;
	struct fastrpc_invoke_context *ctx;
	struct drm_gem_object *gem_obj;
	int err;
	size_t hdr_size;
	size_t initmem_size = FASTRPC_INIT_FILELEN_MAX;

	ctx = qda_fastrpc_context_alloc();
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	err = fastrpc_context_get_id(ctx, qdev);
	if (err) {
		kref_put(&ctx->refcount, qda_fastrpc_context_free);
		return err;
	}

	ctx->type = type;
	ctx->file_priv = file_priv;
	ctx->remote_session_id = qda_file_priv->remote_session_id;

	if (type == FASTRPC_RMID_INIT_CREATE) {
		struct drm_gem_object *initmem_gem_obj;

		if (qda_file_priv->init_mem_gem_obj) {
			drm_gem_object_put(&qda_file_priv->init_mem_gem_obj->base);
			qda_file_priv->init_mem_gem_obj = NULL;
		}

		initmem_gem_obj = qda_gem_create_object(dev, qdev->iommu_mgr,
							initmem_size, file_priv);
		if (IS_ERR(initmem_gem_obj)) {
			err = PTR_ERR(initmem_gem_obj);
			goto err_context_free;
		}

		ctx->init_mem_gem_obj = to_qda_gem_obj(initmem_gem_obj);
		qda_file_priv->init_mem_gem_obj = ctx->init_mem_gem_obj;
	} else if (type == FASTRPC_RMID_INIT_RELEASE) {
		ctx->init_mem_gem_obj = qda_file_priv->init_mem_gem_obj;
	}

	err = qda_fastrpc_prepare_args(ctx, (char __user *)data);
	if (err)
		goto err_context_free;

	err = qda_fastrpc_get_header_size(ctx, &hdr_size);
	if (err)
		goto err_context_free;

	gem_obj = qda_gem_create_object(dev, qdev->iommu_mgr, hdr_size, file_priv);
	if (IS_ERR(gem_obj)) {
		err = PTR_ERR(gem_obj);
		goto err_context_free;
	}

	ctx->msg_gem_obj = to_qda_gem_obj(gem_obj);

	err = qda_fastrpc_invoke_pack(ctx, &msg);
	if (err)
		goto err_context_free;

	err = qda_rpmsg_send_msg(qdev, &msg);
	if (err)
		goto err_context_free;

	err = qda_rpmsg_wait_for_rsp(ctx);
	if (err)
		goto err_context_free;

	err = qda_fastrpc_invoke_unpack(ctx, &msg);
	if (err)
		goto err_context_free;

	err = qda_fastrpc_return_result(ctx, (char __user *)data);
	if (err)
		goto err_context_free;

	fastrpc_context_put_id(ctx, qdev);
	kref_put(&ctx->refcount, qda_fastrpc_context_free);
	return 0;

err_context_free:
	if (type == FASTRPC_RMID_INIT_RELEASE && !err && qda_file_priv->init_mem_gem_obj) {
		drm_gem_object_put(&qda_file_priv->init_mem_gem_obj->base);
		qda_file_priv->init_mem_gem_obj = NULL;
	}

	fastrpc_context_put_id(ctx, qdev);
	kref_put(&ctx->refcount, qda_fastrpc_context_free);
	return err;
}

/**
 * qda_ioctl_init_create() - Create a DSP process
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_init_create)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_init_create(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return fastrpc_invoke(FASTRPC_RMID_INIT_CREATE, dev, data, file_priv);
}

/**
 * qda_release_dsp_process() - Release DSP process resources for a file
 * @qdev: QDA device structure
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_release_dsp_process(struct qda_dev *qdev, struct drm_file *file_priv)
{
	return fastrpc_invoke(FASTRPC_RMID_INIT_RELEASE, &qdev->drm_dev, NULL, file_priv);
}

/**
 * qda_ioctl_mmap() - Map memory to DSP address space
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_mem_map)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_mmap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_mem_map *map_req;

	if (!data)
		return -EINVAL;

	map_req = (struct drm_qda_mem_map *)data;

	if (map_req->pad)
		return -EINVAL;

	switch (map_req->request) {
	case QDA_MAP_REQUEST_LEGACY:
		return fastrpc_invoke(FASTRPC_RMID_INIT_MMAP, dev, data, file_priv);
	case QDA_MAP_REQUEST_ATTR:
		return fastrpc_invoke(FASTRPC_RMID_INIT_MEM_MAP, dev, data, file_priv);
	default:
		drm_err(dev, "Invalid map request type: %u\n", map_req->request);
		return -EINVAL;
	}
}

/**
 * qda_ioctl_munmap() - Unmap memory from DSP address space
 * @dev: DRM device structure
 * @data: User-space data (struct drm_qda_mem_unmap)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_munmap(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_mem_unmap *unmap_req;

	if (!data)
		return -EINVAL;

	unmap_req = (struct drm_qda_mem_unmap *)data;

	switch (unmap_req->request) {
	case QDA_MUNMAP_REQUEST_LEGACY:
		return fastrpc_invoke(FASTRPC_RMID_INIT_MUNMAP, dev, data, file_priv);
	case QDA_MUNMAP_REQUEST_ATTR:
		return fastrpc_invoke(FASTRPC_RMID_INIT_MEM_UNMAP, dev, data, file_priv);
	default:
		drm_err(dev, "Invalid munmap request type: %u\n", unmap_req->request);
		return -EINVAL;
	}
}

/**
 * qda_ioctl_invoke() - Perform a dynamic FastRPC method invocation
 * @dev: DRM device structure
 * @data: User-space data (struct qda_invoke_args)
 * @file_priv: DRM file private data
 *
 * Return: 0 on success, negative error code on failure
 */
int qda_ioctl_invoke(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return fastrpc_invoke(FASTRPC_RMID_INVOKE_DYNAMIC, dev, data, file_priv);
}
