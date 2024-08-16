// SPDX-License-Identifier: GPL-2.0
/*
 * Generic EDAC scrub driver supports controlling the memory
 * scrubbers in the system and the common sysfs scrub interface
 * promotes unambiguous access from the userspace.
 *
 * Copyright (c) 2024 HiSilicon Limited.
 */

#define pr_fmt(fmt)     "EDAC SCRUB: " fmt

#include <linux/edac.h>

static ssize_t addr_range_base_show(struct device *ras_feat_dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u64 base, size;
	int ret;

	ret = ops->read_range(ras_feat_dev->parent, ctx->scrub.private, &base, &size);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%llx\n", base);
}

static ssize_t addr_range_size_show(struct device *ras_feat_dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u64 base, size;
	int ret;

	ret = ops->read_range(ras_feat_dev->parent, ctx->scrub.private, &base, &size);
	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%llx\n", size);
}

static ssize_t addr_range_base_store(struct device *ras_feat_dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u64 base, size;
	int ret;

	ret = ops->read_range(ras_feat_dev->parent, ctx->scrub.private, &base, &size);
	if (ret)
		return ret;

	ret = kstrtou64(buf, 0, &base);
	if (ret < 0)
		return ret;

	ret = ops->write_range(ras_feat_dev->parent, ctx->scrub.private, base, size);
	if (ret)
		return ret;

	return len;
}

static ssize_t addr_range_size_store(struct device *ras_feat_dev,
				     struct device_attribute *attr,
				     const char *buf,
				     size_t len)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u64 base, size;
	int ret;

	ret = ops->read_range(ras_feat_dev->parent, ctx->scrub.private, &base, &size);
	if (ret)
		return ret;

	ret = kstrtou64(buf, 0, &size);
	if (ret < 0)
		return ret;

	ret = ops->write_range(ras_feat_dev->parent, ctx->scrub.private, base, size);
	if (ret)
		return ret;

	return len;
}

static ssize_t enable_background_store(struct device *ras_feat_dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	ret = ops->set_enabled_bg(ras_feat_dev->parent, ctx->scrub.private, enable);
	if (ret)
		return ret;

	return len;
}

static ssize_t enable_background_show(struct device *ras_feat_dev,
				      struct device_attribute *attr, char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	bool enable;
	int ret;

	ret = ops->get_enabled_bg(ras_feat_dev->parent, ctx->scrub.private, &enable);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", enable);
}

static ssize_t enable_on_demand_show(struct device *ras_feat_dev,
				     struct device_attribute *attr, char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	bool enable;
	int ret;

	ret = ops->get_enabled_od(ras_feat_dev->parent, ctx->scrub.private, &enable);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", enable);
}

static ssize_t enable_on_demand_store(struct device *ras_feat_dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	ret = ops->set_enabled_od(ras_feat_dev->parent, ctx->scrub.private, enable);
	if (ret)
		return ret;

	return len;
}

static ssize_t min_cycle_duration_show(struct device *ras_feat_dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u32 val;
	int ret;

	ret = ops->min_cycle_read(ras_feat_dev->parent, ctx->scrub.private, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t max_cycle_duration_show(struct device *ras_feat_dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u32 val;
	int ret;

	ret = ops->max_cycle_read(ras_feat_dev->parent, ctx->scrub.private, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t current_cycle_duration_show(struct device *ras_feat_dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	u32 val;
	int ret;

	ret = ops->cycle_duration_read(ras_feat_dev->parent, ctx->scrub.private, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t current_cycle_duration_store(struct device *ras_feat_dev,
					    struct device_attribute *attr,
					    const char *buf, size_t len)
{
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_scrub_ops *ops = ctx->scrub.scrub_ops;
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret < 0)
		return ret;

	ret = ops->cycle_duration_write(ras_feat_dev->parent, ctx->scrub.private, val);
	if (ret)
		return ret;

	return len;
}

static DEVICE_ATTR_RW(addr_range_base);
static DEVICE_ATTR_RW(addr_range_size);
static DEVICE_ATTR_RW(enable_background);
static DEVICE_ATTR_RW(enable_on_demand);
static DEVICE_ATTR_RO(min_cycle_duration);
static DEVICE_ATTR_RO(max_cycle_duration);
static DEVICE_ATTR_RW(current_cycle_duration);

static struct attribute *scrub_attrs[] = {
	&dev_attr_addr_range_base.attr,
	&dev_attr_addr_range_size.attr,
	&dev_attr_enable_background.attr,
	&dev_attr_enable_on_demand.attr,
	&dev_attr_min_cycle_duration.attr,
	&dev_attr_max_cycle_duration.attr,
	&dev_attr_current_cycle_duration.attr,
	NULL
};

static umode_t scrub_attr_visible(struct kobject *kobj,
				  struct attribute *a, int attr_id)
{
	struct device *ras_feat_dev = kobj_to_dev(kobj);
	struct edac_dev_feat_ctx *ctx;
	const struct edac_scrub_ops *ops;

	ctx = dev_get_drvdata(ras_feat_dev);
	if (!ctx)
		return 0;

	ops = ctx->scrub.scrub_ops;
	if (a == &dev_attr_addr_range_base.attr ||
	    a == &dev_attr_addr_range_size.attr) {
		if (ops->read_range && ops->write_range)
			return a->mode;
		if (ops->read_range)
			return 0444;
		return 0;
	}
	if (a == &dev_attr_enable_background.attr) {
		if (ops->set_enabled_bg && ops->get_enabled_bg)
			return a->mode;
		if (ops->get_enabled_bg)
			return 0444;
		return 0;
	}
	if (a == &dev_attr_enable_on_demand.attr) {
		if (ops->set_enabled_od && ops->get_enabled_od)
			return a->mode;
		if (ops->get_enabled_od)
			return 0444;
		return 0;
	}
	if (a == &dev_attr_min_cycle_duration.attr)
		return ops->min_cycle_read ? a->mode : 0;
	if (a == &dev_attr_max_cycle_duration.attr)
		return ops->max_cycle_read ? a->mode : 0;
	if (a == &dev_attr_current_cycle_duration.attr) {
		if (ops->cycle_duration_read && ops->cycle_duration_write)
			return a->mode;
		if (ops->cycle_duration_read)
			return 0444;
		return 0;
	}

	return 0;
}

static const struct attribute_group scrub_attr_group = {
	.name		= "scrub",
	.attrs		= scrub_attrs,
	.is_visible	= scrub_attr_visible,
};

/**
 * edac_scrub_get_desc - get edac scrub's attr descriptor
 *
 * Returns attribute_group for the scrub feature.
 */
const struct attribute_group *edac_scrub_get_desc(void)
{
	return &scrub_attr_group;
}
