// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
#include <drm/drm_ioctl.h>
#include <drm/drm_gem.h>
#include <drm/qda_accel.h>
#include "qda_drv.h"
#include "qda_ioctl.h"
#include "qda_prime.h"
#include "qda_fastrpc.h"
#include "qda_rpmsg.h"

static int qda_validate_and_get_context(struct drm_device *dev, struct drm_file *file_priv,
					struct qda_dev **qdev, struct qda_user **qda_user)
{
	struct qda_drm_priv *drm_priv = dev->dev_private;
	struct qda_file_priv *qda_file_priv;

	if (!drm_priv)
		return -EINVAL;

	*qdev = drm_priv->qdev;
	if (!*qdev)
		return -EINVAL;

	qda_file_priv = (struct qda_file_priv *)file_priv->driver_priv;
	if (!qda_file_priv || !qda_file_priv->qda_user)
		return -EINVAL;

	*qda_user = qda_file_priv->qda_user;

	return 0;
}

int qda_ioctl_query(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct qda_dev *qdev;
	struct qda_user *qda_user;
	struct drm_qda_query *args = data;
	int ret;

	ret = qda_validate_and_get_context(dev, file_priv, &qdev, &qda_user);
	if (ret)
		return ret;

	strscpy(args->dsp_name, qdev->dsp_name, sizeof(args->dsp_name));

	return 0;
}

int qda_ioctl_gem_create(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_create *args = data;
	struct drm_gem_object *gem_obj;
	struct qda_drm_priv *drm_priv;

	drm_priv = get_drm_priv_from_device(dev);
	if (!drm_priv || !drm_priv->iommu_mgr)
		return -EINVAL;

	gem_obj = qda_gem_create_object(dev, drm_priv->iommu_mgr, args->size, file_priv);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	return qda_gem_create_handle(file_priv, gem_obj, &args->handle);
}

int qda_ioctl_gem_mmap_offset(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	struct drm_qda_gem_mmap_offset *args = data;
	struct drm_gem_object *gem_obj;
	int ret;

	gem_obj = qda_gem_lookup_object(file_priv, args->handle);
	if (IS_ERR(gem_obj))
		return PTR_ERR(gem_obj);

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret == 0)
		args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);

	drm_gem_object_put(gem_obj);
	return ret;
}

int qda_ioctl_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv, int prime_fd,
				 u32 *handle)
{
	return qda_prime_fd_to_handle(dev, file_priv, prime_fd, handle);
}

static int fastrpc_context_get_id(struct fastrpc_invoke_context *ctx, struct qda_dev *qdev)
{
	int ret;
	u32 id;

	if (!qdev)
		return -EINVAL;

	if (atomic_read(&qdev->removing))
		return -ENODEV;

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
	struct qda_dev *qdev;
	struct qda_user *qda_user;
	struct qda_msg msg;
	struct fastrpc_invoke_context *ctx;
	struct drm_gem_object *gem_obj;
	int err;
	size_t hdr_size;

	err = qda_validate_and_get_context(dev, file_priv, &qdev, &qda_user);
	if (err)
		return err;

	ctx = fastrpc_context_alloc();
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	err = fastrpc_context_get_id(ctx, qdev);
	if (err) {
		kref_put(&ctx->refcount, fastrpc_context_free);
		return err;
	}

	ctx->type = type;
	ctx->file_priv = file_priv;
	ctx->client_id = qda_user->client_id;

	err = fastrpc_prepare_args(ctx, (char __user *)data);
	if (err)
		goto err_context_free;

	err = fastrpc_get_header_size(ctx, &hdr_size);
	if (err)
		goto err_context_free;

	gem_obj = qda_gem_create_object(qdev->drm_dev,
					qdev->drm_priv->iommu_mgr,
					hdr_size, file_priv);
	if (IS_ERR(gem_obj)) {
		err = PTR_ERR(gem_obj);
		goto err_context_free;
	}

	ctx->msg_gem_obj = to_qda_gem_obj(gem_obj);

	err = fastrpc_internal_invoke_pack(ctx, &msg);
	if (err)
		goto err_context_free;

	err = qda_rpmsg_send_msg(qdev, &msg);
	if (err)
		goto err_context_free;

	err = qda_rpmsg_wait_for_rsp(ctx);
	if (err)
		goto err_context_free;

	err = fastrpc_internal_invoke_unpack(ctx, &msg);
	if (err)
		goto err_context_free;

err_context_free:
	fastrpc_context_put_id(ctx, qdev);
	kref_put(&ctx->refcount, fastrpc_context_free);

	return err;
}

int qda_ioctl_attach(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return fastrpc_invoke(FASTRPC_RMID_INIT_ATTACH, dev, data, file_priv);
}

int fastrpc_release_current_dsp_process(struct qda_dev *qdev, struct drm_file *file_priv)
{
	return fastrpc_invoke(FASTRPC_RMID_INIT_RELEASE, qdev->drm_dev, NULL, file_priv);
}

int qda_ioctl_invoke(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
	return fastrpc_invoke(FASTRPC_RMID_INVOKE_DYNAMIC, dev, data, file_priv);
}
