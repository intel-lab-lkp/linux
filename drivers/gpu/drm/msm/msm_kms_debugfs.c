// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013-2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "disp/msm_disp_snapshot.h"

static int msm_fb_show(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = m->private;
	struct drm_device *dev = node->minor->dev;
	struct drm_framebuffer *fb, *fbdev_fb = NULL;

	if (dev->fb_helper && dev->fb_helper->fb) {
		seq_puts(m, "fbcon ");
		fbdev_fb = dev->fb_helper->fb;
		msm_framebuffer_describe(fbdev_fb, m);
	}

	mutex_lock(&dev->mode_config.fb_lock);
	list_for_each_entry(fb, &dev->mode_config.fb_list, head) {
		if (fb == fbdev_fb)
			continue;

		seq_puts(m, "user ");
		msm_framebuffer_describe(fb, m);
	}
	mutex_unlock(&dev->mode_config.fb_lock);

	return 0;
}

static struct drm_info_list msm_kms_debugfs_list[] = {
		{ "fb", msm_fb_show },
};

/*
 * Display Snapshot:
 */

static int msm_kms_show(struct seq_file *m, void *arg)
{
	struct drm_printer p = drm_seq_file_printer(m);
	struct msm_disp_state *state = m->private;

	msm_disp_state_print(state, &p);

	return 0;
}

static int msm_kms_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct msm_disp_state *state = m->private;

	msm_disp_state_free(state);

	return single_release(inode, file);
}

static int msm_kms_open(struct inode *inode, struct file *file)
{
	struct drm_device *dev = inode->i_private;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_disp_state *state;
	int ret;

	if (!priv->kms)
		return -ENODEV;

	ret = mutex_lock_interruptible(&priv->kms->dump_mutex);
	if (ret)
		return ret;

	state = msm_disp_snapshot_state_sync(priv->kms);

	mutex_unlock(&priv->kms->dump_mutex);

	if (IS_ERR(state))
		return PTR_ERR(state);

	ret = single_open(file, msm_kms_show, state);
	if (ret) {
		msm_disp_state_free(state);
		return ret;
	}

	return 0;
}

static const struct file_operations msm_kms_fops = {
	.owner = THIS_MODULE,
	.open = msm_kms_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = msm_kms_release,
};

void msm_kms_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct msm_drm_private *priv = dev->dev_private;

	if (!priv->kms)
		return;

	drm_debugfs_create_files(msm_kms_debugfs_list,
				 ARRAY_SIZE(msm_kms_debugfs_list),
				 minor->debugfs_root, minor);
	debugfs_create_file("kms", 0400, minor->debugfs_root,
			    dev, &msm_kms_fops);

	if (priv->kms->funcs->debugfs_init)
		priv->kms->funcs->debugfs_init(priv->kms, minor);

}
#endif
