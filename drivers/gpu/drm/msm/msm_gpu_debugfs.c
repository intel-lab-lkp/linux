// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013-2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/fault-inject.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>

#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_kms.h"
#include "disp/msm_disp_snapshot.h"

/*
 * GPU Snapshot:
 */

struct msm_gpu_show_priv {
	struct msm_gpu_state *state;
	struct drm_device *dev;
};

static int msm_gpu_show(struct seq_file *m, void *arg)
{
	struct drm_printer p = drm_seq_file_printer(m);
	struct msm_gpu_show_priv *show_priv = m->private;
	struct msm_drm_private *priv = show_priv->dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;
	int ret;

	ret = mutex_lock_interruptible(&gpu->lock);
	if (ret)
		return ret;

	drm_printf(&p, "%s Status:\n", gpu->name);
	gpu->funcs->show(gpu, show_priv->state, &p);

	mutex_unlock(&gpu->lock);

	return 0;
}

static int msm_gpu_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct msm_gpu_show_priv *show_priv = m->private;
	struct msm_drm_private *priv = show_priv->dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;

	mutex_lock(&gpu->lock);
	gpu->funcs->gpu_state_put(show_priv->state);
	mutex_unlock(&gpu->lock);

	kfree(show_priv);

	return single_release(inode, file);
}

static int msm_gpu_open(struct inode *inode, struct file *file)
{
	struct drm_device *dev = inode->i_private;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;
	struct msm_gpu_show_priv *show_priv;
	int ret;

	if (!gpu || !gpu->funcs->gpu_state_get)
		return -ENODEV;

	show_priv = kmalloc(sizeof(*show_priv), GFP_KERNEL);
	if (!show_priv)
		return -ENOMEM;

	ret = mutex_lock_interruptible(&gpu->lock);
	if (ret)
		goto free_priv;

	pm_runtime_get_sync(&gpu->pdev->dev);
	msm_gpu_hw_init(gpu);
	show_priv->state = gpu->funcs->gpu_state_get(gpu);
	pm_runtime_put_sync(&gpu->pdev->dev);

	mutex_unlock(&gpu->lock);

	if (IS_ERR(show_priv->state)) {
		ret = PTR_ERR(show_priv->state);
		goto free_priv;
	}

	show_priv->dev = dev;

	ret = single_open(file, msm_gpu_show, show_priv);
	if (ret)
		goto free_priv;

	return 0;

free_priv:
	kfree(show_priv);
	return ret;
}

static const struct file_operations msm_gpu_fops = {
	.owner = THIS_MODULE,
	.open = msm_gpu_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = msm_gpu_release,
};

/*
 * Return the number of microseconds to wait until stall-on-fault is
 * re-enabled. If 0 then it is already enabled or will be re-enabled on the
 * next submit (unless there's a leftover devcoredump). This is useful for
 * kernel tests that intentionally produce a fault and check the devcoredump to
 * wait until the cooldown period is over.
 */

static int
stall_reenable_time_get(void *data, u64 *val)
{
	struct msm_drm_private *priv = data;
	unsigned long irq_flags;

	spin_lock_irqsave(&priv->fault_stall_lock, irq_flags);

	if (priv->stall_enabled)
		*val = 0;
	else
		*val = max(ktime_us_delta(priv->stall_reenable_time, ktime_get()), 0);

	spin_unlock_irqrestore(&priv->fault_stall_lock, irq_flags);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(stall_reenable_time_fops,
			 stall_reenable_time_get, NULL,
			 "%lld\n");

void msm_gpu_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct msm_drm_private *priv = dev->dev_private;
	struct dentry *gpu_devfreq;

	if (!priv->gpu_pdev)
		return;

	debugfs_create_file("gpu", 0400, minor->debugfs_root,
			    dev, &msm_gpu_fops);

	debugfs_create_u32("hangcheck_period_ms", 0600, minor->debugfs_root,
			   &priv->hangcheck_period);

	debugfs_create_bool("disable_err_irq", 0600, minor->debugfs_root,
			    &priv->disable_err_irq);

	debugfs_create_file("stall_reenable_time_us", 0400, minor->debugfs_root,
			    priv, &stall_reenable_time_fops);

	gpu_devfreq = debugfs_create_dir("devfreq", minor->debugfs_root);

	debugfs_create_bool("idle_clamp", 0600, gpu_devfreq,
			    &priv->gpu_clamp_to_idle);

	debugfs_create_u32("upthreshold", 0600, gpu_devfreq,
			   &priv->gpu_devfreq_config.upthreshold);

	debugfs_create_u32("downdifferential", 0600, gpu_devfreq,
			   &priv->gpu_devfreq_config.downdifferential);
}

static void late_init_minor(struct drm_minor *minor)
{
	int ret;

	if (!minor)
		return;

	ret = msm_rd_debugfs_init(minor);
	if (ret) {
		drm_err(minor->dev, "could not install rd debugfs\n");
		return;
	}

	ret = msm_perf_debugfs_init(minor);
	if (ret) {
		drm_err(minor->dev, "could not install perf debugfs\n");
		return;
	}
}

void msm_gpu_debugfs_late_init(struct drm_device *dev)
{
	struct msm_drm_private *priv = dev->dev_private;

	if (!priv->gpu_pdev)
		return;

	late_init_minor(dev->primary);

	late_init_minor(dev->render);
}
#endif
