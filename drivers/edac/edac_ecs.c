// SPDX-License-Identifier: GPL-2.0
/*
 * ECS driver supporting controlling on die error check scrub
 * (e.g. DDR5 ECS). The common sysfs ECS interface promotes
 * unambiguous access from the userspace.
 *
 * Copyright (c) 2024 HiSilicon Limited.
 */

#define pr_fmt(fmt)     "EDAC ECS: " fmt

#include <linux/edac.h>

#define EDAC_ECS_FRU_NAME "ecs_fru"

enum edac_ecs_attributes {
	ECS_LOG_ENTRY_TYPE,
	ECS_LOG_ENTRY_TYPE_PER_DRAM,
	ECS_LOG_ENTRY_TYPE_PER_MEMORY_MEDIA,
	ECS_MODE,
	ECS_MODE_COUNTS_ROWS,
	ECS_MODE_COUNTS_CODEWORDS,
	ECS_RESET,
	ECS_THRESHOLD,
	ECS_MAX_ATTRS
};

struct edac_ecs_dev_attr {
	struct device_attribute dev_attr;
	int fru_id;
};

struct edac_ecs_fru_context {
	char name[EDAC_FEAT_NAME_LEN];
	struct edac_ecs_dev_attr ecs_dev_attr[ECS_MAX_ATTRS];
	struct attribute *ecs_attrs[ECS_MAX_ATTRS + 1];
	struct attribute_group group;
};

struct edac_ecs_context {
	u16 num_media_frus;
	struct edac_ecs_fru_context *fru_ctxs;
};

#define to_ecs_dev_attr(_dev_attr)	\
	container_of(_dev_attr, struct edac_ecs_dev_attr, dev_attr)

static ssize_t log_entry_type_show(struct device *ras_feat_dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	u32 val;
	int ret;

	ret = ops->get_log_entry_type(ras_feat_dev->parent, ctx->ecs.private,
				      ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t log_entry_type_store(struct device *ras_feat_dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret < 0)
		return ret;

	ret = ops->set_log_entry_type(ras_feat_dev->parent, ctx->ecs.private,
				      ecs_dev_attr->fru_id, val);
	if (ret)
		return ret;

	return len;
}

static ssize_t log_entry_type_per_dram_show(struct device *ras_feat_dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	u32 val;
	int ret;

	ret = ops->get_log_entry_type_per_dram(ras_feat_dev->parent, ctx->ecs.private,
					       ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t log_entry_type_per_memory_media_show(struct device *ras_feat_dev,
						    struct device_attribute *attr,
						    char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	u32 val;
	int ret;

	ret = ops->get_log_entry_type_per_memory_media(ras_feat_dev->parent,
						       ctx->ecs.private,
						       ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t mode_show(struct device *ras_feat_dev,
			 struct device_attribute *attr,
			 char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	u32 val;
	int ret;

	ret = ops->get_mode(ras_feat_dev->parent, ctx->ecs.private,
			    ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t mode_store(struct device *ras_feat_dev,
			  struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret < 0)
		return ret;

	ret = ops->set_mode(ras_feat_dev->parent, ctx->ecs.private,
			    ecs_dev_attr->fru_id, val);
	if (ret)
		return ret;

	return len;
}

static ssize_t mode_counts_rows_show(struct device *ras_feat_dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	u32 val;
	int ret;

	ret = ops->get_mode_counts_rows(ras_feat_dev->parent, ctx->ecs.private,
					ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t mode_counts_codewords_show(struct device *ras_feat_dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	u32 val;
	int ret;

	ret = ops->get_mode_counts_codewords(ras_feat_dev->parent, ctx->ecs.private,
					     ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t reset_store(struct device *ras_feat_dev,
			   struct device_attribute *attr,
			   const char *buf, size_t len)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret < 0)
		return ret;

	ret = ops->reset(ras_feat_dev->parent, ctx->ecs.private,
			 ecs_dev_attr->fru_id, val);
	if (ret)
		return ret;

	return len;
}

static ssize_t threshold_show(struct device *ras_feat_dev,
			      struct device_attribute *attr, char *buf)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	int ret;
	u32 val;

	ret = ops->get_threshold(ras_feat_dev->parent, ctx->ecs.private,
				 ecs_dev_attr->fru_id, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t threshold_store(struct device *ras_feat_dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct edac_ecs_dev_attr *ecs_dev_attr = to_ecs_dev_attr(attr);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;
	long val;
	int ret;

	ret = kstrtol(buf, 0, &val);
	if (ret < 0)
		return ret;

	ret = ops->set_threshold(ras_feat_dev->parent, ctx->ecs.private,
				 ecs_dev_attr->fru_id, val);
	if (ret)
		return ret;

	return len;
}

static umode_t ecs_attr_visible(struct kobject *kobj,
				struct attribute *a, int attr_id)
{
	struct device *ras_feat_dev = kobj_to_dev(kobj);
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ecs_ops *ops = ctx->ecs.ecs_ops;

	switch (attr_id) {
	case ECS_LOG_ENTRY_TYPE:
		if (ops->get_log_entry_type && ops->set_log_entry_type)
			return a->mode;
		if (ops->get_log_entry_type)
			return 0444;
		return 0;
	case ECS_LOG_ENTRY_TYPE_PER_DRAM:
		return ops->get_log_entry_type_per_dram ? a->mode : 0;
	case ECS_LOG_ENTRY_TYPE_PER_MEMORY_MEDIA:
		return ops->get_log_entry_type_per_memory_media ? a->mode : 0;
	case ECS_MODE:
		if (ops->get_mode && ops->set_mode)
			return a->mode;
		if (ops->get_mode)
			return 0444;
		return 0;
	case ECS_MODE_COUNTS_ROWS:
		return ops->get_mode_counts_rows ? a->mode : 0;
	case ECS_MODE_COUNTS_CODEWORDS:
		return ops->get_mode_counts_codewords ? a->mode : 0;
	case ECS_RESET:
		return ops->reset ? a->mode : 0;
	case ECS_THRESHOLD:
		if (ops->get_threshold && ops->set_threshold)
			return a->mode;
		if (ops->get_threshold)
			return 0444;
		return 0;
	default:
		return 0;
	}
}

#define EDAC_ECS_ATTR_RO(_name, _fru_id)       \
	((struct edac_ecs_dev_attr) { .dev_attr = __ATTR_RO(_name), \
				     .fru_id = _fru_id })

#define EDAC_ECS_ATTR_WO(_name, _fru_id)       \
	((struct edac_ecs_dev_attr) { .dev_attr = __ATTR_WO(_name), \
				     .fru_id = _fru_id })

#define EDAC_ECS_ATTR_RW(_name, _fru_id)       \
	((struct edac_ecs_dev_attr) { .dev_attr = __ATTR_RW(_name), \
				     .fru_id = _fru_id })

static int ecs_create_desc(struct device *ecs_dev,
			   const struct attribute_group **attr_groups,
			   u16 num_media_frus)
{
	struct edac_ecs_context *ecs_ctx;
	u32 fru;

	ecs_ctx = devm_kzalloc(ecs_dev, sizeof(*ecs_ctx), GFP_KERNEL);
	if (!ecs_ctx)
		return -ENOMEM;

	ecs_ctx->num_media_frus = num_media_frus;
	ecs_ctx->fru_ctxs = devm_kcalloc(ecs_dev, num_media_frus,
					 sizeof(*ecs_ctx->fru_ctxs),
					 GFP_KERNEL);
	if (!ecs_ctx->fru_ctxs)
		return -ENOMEM;

	for (fru = 0; fru < num_media_frus; fru++) {
		struct edac_ecs_fru_context *fru_ctx = &ecs_ctx->fru_ctxs[fru];
		struct attribute_group *group = &fru_ctx->group;
		int i;

		fru_ctx->ecs_dev_attr[0] = EDAC_ECS_ATTR_RW(log_entry_type, fru);
		fru_ctx->ecs_dev_attr[1] = EDAC_ECS_ATTR_RO(log_entry_type_per_dram, fru);
		fru_ctx->ecs_dev_attr[2] = EDAC_ECS_ATTR_RO(log_entry_type_per_memory_media, fru);
		fru_ctx->ecs_dev_attr[3] = EDAC_ECS_ATTR_RW(mode, fru);
		fru_ctx->ecs_dev_attr[4] = EDAC_ECS_ATTR_RO(mode_counts_rows, fru);
		fru_ctx->ecs_dev_attr[5] = EDAC_ECS_ATTR_RO(mode_counts_codewords, fru);
		fru_ctx->ecs_dev_attr[6] = EDAC_ECS_ATTR_WO(reset, fru);
		fru_ctx->ecs_dev_attr[7] = EDAC_ECS_ATTR_RW(threshold, fru);
		for (i = 0; i < ECS_MAX_ATTRS; i++)
			fru_ctx->ecs_attrs[i] = &fru_ctx->ecs_dev_attr[i].dev_attr.attr;

		sprintf(fru_ctx->name, "%s%d", EDAC_ECS_FRU_NAME, fru);
		group->name = fru_ctx->name;
		group->attrs = fru_ctx->ecs_attrs;
		group->is_visible  = ecs_attr_visible;

		attr_groups[fru] = group;
	}

	return 0;
}

/**
 * edac_ecs_get_desc - get edac ecs descriptors
 * @ecs_dev: client ecs device
 * @attr_groups: pointer to attrribute group container
 * @num_media_frus: number of media FRUs in the device
 *
 * Returns 0 on success, error otherwise.
 */
int edac_ecs_get_desc(struct device *ecs_dev,
		      const struct attribute_group **attr_groups,
		      u16 num_media_frus)
{
	if (!ecs_dev || !attr_groups || !num_media_frus)
		return -EINVAL;

	return ecs_create_desc(ecs_dev, attr_groups, num_media_frus);
}
