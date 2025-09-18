// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013-2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/fault-inject.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>

#include "msm_gem.h"

/*
 * Other debugfs:
 */

static unsigned long last_shrink_freed;

static int
shrink_get(void *data, u64 *val)
{
	*val = last_shrink_freed;

	return 0;
}

static int
shrink_set(void *data, u64 val)
{
	struct drm_device *dev = data;

	last_shrink_freed = msm_gem_shrinker_shrink(dev, val);

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(shrink_fops,
			 shrink_get, shrink_set,
			 "0x%08llx\n");

static int msm_gem_show(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = m->private;
	struct drm_device *dev = node->minor->dev;
	struct msm_drm_private *priv = dev->dev_private;
	int ret;

	ret = mutex_lock_interruptible(&priv->obj_lock);
	if (ret)
		return ret;

	msm_gem_describe_objects(&priv->objects, m);

	mutex_unlock(&priv->obj_lock);

	return 0;
}

static int msm_mm_show(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = m->private;
	struct drm_device *dev = node->minor->dev;
	struct drm_printer p = drm_seq_file_printer(m);

	drm_mm_print(&dev->vma_offset_manager->vm_addr_space_mm, &p);

	return 0;
}

static struct drm_info_list msm_debugfs_list[] = {
		{"gem", msm_gem_show},
		{ "mm", msm_mm_show },
};

void msm_gem_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;

	drm_debugfs_create_files(msm_debugfs_list,
				 ARRAY_SIZE(msm_debugfs_list),
				 minor->debugfs_root, minor);

	debugfs_create_file("shrink", 0700, minor->debugfs_root,
		dev, &shrink_fops);

	fault_create_debugfs_attr("fail_gem_alloc", minor->debugfs_root,
				  &fail_gem_alloc);
	fault_create_debugfs_attr("fail_gem_iova", minor->debugfs_root,
				  &fail_gem_iova);
}
#endif

