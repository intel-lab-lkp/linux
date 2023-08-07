// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Igalia S.L.
 */

#include <linux/sched/clock.h>
#include <linux/sysfs.h>

#include "v3d_drv.h"

static u64
v3d_sysfs_emit_total_runtime(struct v3d_dev *v3d, enum v3d_queue queue, char *buf)
{
	u64 timestamp = local_clock();
	u64 active_runtime;

	if (v3d->queue[queue].start_ns)
		active_runtime = timestamp - v3d->queue[queue].start_ns;
	else
		active_runtime = 0;

	return sysfs_emit(buf, "timestamp: %llu %s: %llu ns\n",
			  timestamp,
			  v3d_queue_to_string(queue),
			  v3d->queue[queue].enabled_ns + active_runtime);
}

static ssize_t
bin_queue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);

	return v3d_sysfs_emit_total_runtime(v3d, V3D_BIN, buf);
}
static DEVICE_ATTR_RO(bin_queue);

static ssize_t
render_queue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);

	return v3d_sysfs_emit_total_runtime(v3d, V3D_RENDER, buf);
}
static DEVICE_ATTR_RO(render_queue);

static ssize_t
tfu_queue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);

	return v3d_sysfs_emit_total_runtime(v3d, V3D_TFU, buf);
}
static DEVICE_ATTR_RO(tfu_queue);

static ssize_t
csd_queue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);

	return v3d_sysfs_emit_total_runtime(v3d, V3D_CSD, buf);
}
static DEVICE_ATTR_RO(csd_queue);

static ssize_t
cache_clean_queue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct v3d_dev *v3d = to_v3d_dev(drm);

	return v3d_sysfs_emit_total_runtime(v3d, V3D_CACHE_CLEAN, buf);
}
static DEVICE_ATTR_RO(cache_clean_queue);

static struct attribute *v3d_sysfs_entries[] = {
	&dev_attr_bin_queue.attr,
	&dev_attr_render_queue.attr,
	&dev_attr_tfu_queue.attr,
	&dev_attr_csd_queue.attr,
	&dev_attr_cache_clean_queue.attr,
	NULL,
};

static struct attribute_group v3d_sysfs_attr_group = {
	.attrs = v3d_sysfs_entries,
};

int
v3d_sysfs_init(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &v3d_sysfs_attr_group);
}

void
v3d_sysfs_destroy(struct device *dev)
{
	return sysfs_remove_group(&dev->kobj, &v3d_sysfs_attr_group);
}
