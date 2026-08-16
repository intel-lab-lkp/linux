// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Manager DMA Buffer Support
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */
#include <linux/capability.h>
#include <linux/dma-buf.h>
#include <linux/fpga/fpga-dmabuf.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <uapi/linux/fpga.h>

struct fpga_dmabuf_priv {
	struct kref ref;
	struct mutex lock; /* serializes ioctl against teardown */
	bool dead;
	struct fpga_manager *mgr;
	struct miscdevice miscdev;
};

static void fpga_dmabuf_free(struct kref *ref)
{
	struct fpga_dmabuf_priv *priv = container_of(ref,
					struct fpga_dmabuf_priv, ref);

	put_device(&priv->mgr->dev);
	kfree(priv->miscdev.name);
	kfree(priv);
}

static int fpga_dmabuf_load(struct fpga_manager *mgr, int buffd)
{
	struct fpga_image_info *info;
	struct dma_buf_attachment *attach;
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	int ret;

	dmabuf = dma_buf_get(buffd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	info = fpga_image_info_alloc(mgr->dev.parent);
	if (!info) {
		ret = -ENOMEM;
		goto err_put;
	}

	attach = dma_buf_attach(dmabuf, mgr->dev.parent);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto err_free_info;
	}

	sgt = dma_buf_map_attachment_unlocked(attach, DMA_TO_DEVICE);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	info->sgt = sgt;

	ret = fpga_mgr_lock(mgr);
	if (ret)
		goto err_unmap;

	ret = fpga_mgr_load(mgr, info);
	fpga_mgr_unlock(mgr);

err_unmap:
	dma_buf_unmap_attachment_unlocked(attach, sgt, DMA_TO_DEVICE);
err_detach:
	dma_buf_detach(dmabuf, attach);
err_free_info:
	fpga_image_info_free(info);
err_put:
	dma_buf_put(dmabuf);

	return ret;
}

static int fpga_dmabuf_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct fpga_dmabuf_priv *priv;

	priv = container_of(miscdev, struct fpga_dmabuf_priv, miscdev);
	kref_get(&priv->ref);
	file->private_data = priv;

	return 0;
}

static int fpga_dmabuf_release(struct inode *inode, struct file *file)
{
	struct fpga_dmabuf_priv *priv = file->private_data;

	kref_put(&priv->ref, fpga_dmabuf_free);
	return 0;
}

static long fpga_dmabuf_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct fpga_dmabuf_priv *priv = file->private_data;
	int buffd;
	int ret;

	switch (cmd) {
	case FPGA_IOCTL_LOAD_DMA_BUF:
		if (!capable(CAP_SYS_RAWIO))
			return -EPERM;
		if (copy_from_user(&buffd, (void __user *)arg, sizeof(buffd)))
			return -EFAULT;

		mutex_lock(&priv->lock);
		if (priv->dead) {
			mutex_unlock(&priv->lock);
			return -ENODEV;
		}
		ret = fpga_dmabuf_load(priv->mgr, buffd);
		mutex_unlock(&priv->lock);
		return ret;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations fpga_dmabuf_fops = {
	.owner		= THIS_MODULE,
	.open		= fpga_dmabuf_open,
	.release	= fpga_dmabuf_release,
	.unlocked_ioctl	= fpga_dmabuf_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

int fpga_dmabuf_register(struct fpga_manager *mgr)
{
	struct fpga_dmabuf_priv *priv;
	int ret;

	priv = kzalloc_obj(*priv);
	if (!priv)
		return -ENOMEM;

	kref_init(&priv->ref);
	mutex_init(&priv->lock);
	get_device(&mgr->dev);
	priv->mgr = mgr;
	priv->miscdev.minor = MISC_DYNAMIC_MINOR;
	priv->miscdev.name = kstrdup(dev_name(&mgr->dev), GFP_KERNEL);
	if (!priv->miscdev.name) {
		put_device(&mgr->dev);
		kfree(priv);
		return -ENOMEM;
	}
	priv->miscdev.fops = &fpga_dmabuf_fops;
	priv->miscdev.parent = &mgr->dev;

	ret = misc_register(&priv->miscdev);
	if (ret) {
		kfree(priv->miscdev.name);
		put_device(&mgr->dev);
		kfree(priv);
		return ret;
	}

	mgr->dmabuf_priv = priv;

	return 0;
}
EXPORT_SYMBOL_GPL(fpga_dmabuf_register);

void fpga_dmabuf_unregister(struct fpga_manager *mgr)
{
	struct fpga_dmabuf_priv *priv = mgr->dmabuf_priv;

	if (!priv)
		return;

	mutex_lock(&priv->lock);
	priv->dead = true;
	mutex_unlock(&priv->lock);

	misc_deregister(&priv->miscdev);
	mgr->dmabuf_priv = NULL;
	kref_put(&priv->ref, fpga_dmabuf_free);
}
EXPORT_SYMBOL_GPL(fpga_dmabuf_unregister);

MODULE_IMPORT_NS("DMA_BUF");
MODULE_DESCRIPTION("FPGA DMA Buffer Interface");
MODULE_LICENSE("GPL");
