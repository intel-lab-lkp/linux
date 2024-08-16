// SPDX-License-Identifier: GPL-2.0
/*
 * Generic EDAC PPR driver supports controlling the memory
 * device with Post Package Repair (PPR) feature in the system
 * and the common sysfs PPR control interface promotes unambiguous
 * access from the userspace.
 *
 * Copyright (c) 2024 HiSilicon Limited.
 */

#define pr_fmt(fmt)     "EDAC PPR: " fmt

#include <linux/edac.h>

enum edac_ppr_attributes {
	PPR_PERSIST_MODE_AVAIL,
	PPR_PERSIST_MODE,
	PPR_DPA_SUPPORT,
	PPR_SAFE_IN_USE,
	PPR_HPA,
	PPR_DPA,
	PPR_MAX_ATTRS
};

struct edac_ppr_dev_attr {
	struct device_attribute dev_attr;
	u8 instance;
};

struct edac_ppr_context {
	char name[EDAC_FEAT_NAME_LEN];
	struct edac_ppr_dev_attr ppr_dev_attr[PPR_MAX_ATTRS];
	struct attribute *ppr_attrs[PPR_MAX_ATTRS + 1];
	struct attribute_group group;
};

#define to_ppr_dev_attr(_dev_attr)      \
		container_of(_dev_attr, struct edac_ppr_dev_attr, dev_attr)

static ssize_t persist_mode_avail_show(struct device *ras_feat_dev,
				       struct device_attribute *attr, char *buf)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;

	return ops->get_persist_mode_avail(ras_feat_dev->parent,
					   ctx->ppr[inst].private, buf);
}

static ssize_t persist_mode_show(struct device *ras_feat_dev,
				 struct device_attribute *attr, char *buf)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;
	u32 mode;
	int ret;

	ret = ops->get_persist_mode(ras_feat_dev->parent, ctx->ppr[inst].private, &mode);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", mode);
}

static ssize_t persist_mode_store(struct device *ras_feat_dev,
				  struct device_attribute *attr,
				  const char *buf,
				  size_t len)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;
	long mode;
	int ret;

	ret = kstrtol(buf, 0, &mode);
	if (ret < 0)
		return ret;

	ret = ops->set_persist_mode(ras_feat_dev->parent, ctx->ppr[inst].private, mode);
	if (ret)
		return ret;

	return len;
}

static ssize_t dpa_support_show(struct device *ras_feat_dev,
				struct device_attribute *attr, char *buf)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;
	int ret;
	u32 val;

	ret = ops->get_dpa_support(ras_feat_dev->parent, ctx->ppr[inst].private, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t ppr_safe_when_in_use_show(struct device *ras_feat_dev,
					 struct device_attribute *attr, char *buf)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;
	int ret;
	u32 val;

	ret = ops->get_ppr_safe_when_in_use(ras_feat_dev->parent,
					    ctx->ppr[inst].private, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t repair_hpa_store(struct device *ras_feat_dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;
	u64 hpa;
	int ret;

	ret = kstrtou64(buf, 0, &hpa);
	if (ret < 0)
		return ret;

	ret = ops->do_ppr(ras_feat_dev->parent, ctx->ppr[inst].private, true, hpa);
	if (ret)
		return ret;

	return len;
}

static ssize_t repair_dpa_store(struct device *ras_feat_dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;
	u64 dpa;
	int ret;

	ret = kstrtou64(buf, 0, &dpa);
	if (ret < 0)
		return ret;

	ret = ops->do_ppr(ras_feat_dev->parent, ctx->ppr[inst].private, 0, dpa);
	if (ret)
		return ret;

	return len;
}

static umode_t ppr_attr_visible(struct kobject *kobj,
				struct attribute *a, int attr_id)
{
	struct device *ras_feat_dev = kobj_to_dev(kobj);
	struct device_attribute *dev_attr =
				container_of(a, struct device_attribute, attr);
	u8 inst = ((struct edac_ppr_dev_attr *)to_ppr_dev_attr(dev_attr))->instance;
	struct edac_dev_feat_ctx *ctx = dev_get_drvdata(ras_feat_dev);
	const struct edac_ppr_ops *ops = ctx->ppr[inst].ppr_ops;

	switch (attr_id) {
	case PPR_PERSIST_MODE_AVAIL:
		return ops->get_persist_mode_avail ? a->mode : 0;
	case PPR_PERSIST_MODE:
		if (ops->get_persist_mode && ops->set_persist_mode)
			return a->mode;
		if (ops->get_persist_mode)
			return 0444;
		return 0;
	case PPR_DPA_SUPPORT:
		return ops->get_dpa_support ? a->mode : 0;
	case PPR_SAFE_IN_USE:
		return ops->get_ppr_safe_when_in_use ? a->mode : 0;
	case PPR_HPA:
	case PPR_DPA:
		return ops->do_ppr ? a->mode : 0;
	default:
		return 0;
	}
}

#define EDAC_PPR_ATTR_RO(_name, _instance)       \
	((struct edac_ppr_dev_attr) { .dev_attr = __ATTR_RO(_name), \
				     .instance = _instance })

#define EDAC_PPR_ATTR_WO(_name, _instance)       \
	((struct edac_ppr_dev_attr) { .dev_attr = __ATTR_WO(_name), \
				     .instance = _instance })

#define EDAC_PPR_ATTR_RW(_name, _instance)       \
	((struct edac_ppr_dev_attr) { .dev_attr = __ATTR_RW(_name), \
				     .instance = _instance })

static int ppr_create_desc(struct device *ppr_dev,
			   const struct attribute_group **attr_groups,
			   u8 instance)
{
	struct edac_ppr_context *ppr_ctx;
	struct attribute_group *group;
	int i;

	ppr_ctx = devm_kzalloc(ppr_dev, sizeof(*ppr_ctx), GFP_KERNEL);
	if (!ppr_ctx)
		return -ENOMEM;

	group = &ppr_ctx->group;
	ppr_ctx->ppr_dev_attr[0] = EDAC_PPR_ATTR_RO(persist_mode_avail, instance);
	ppr_ctx->ppr_dev_attr[1] = EDAC_PPR_ATTR_RW(persist_mode, instance);
	ppr_ctx->ppr_dev_attr[2] = EDAC_PPR_ATTR_RO(dpa_support, instance);
	ppr_ctx->ppr_dev_attr[3] = EDAC_PPR_ATTR_RO(ppr_safe_when_in_use, instance);
	ppr_ctx->ppr_dev_attr[4] = EDAC_PPR_ATTR_WO(repair_hpa, instance);
	ppr_ctx->ppr_dev_attr[5] = EDAC_PPR_ATTR_WO(repair_dpa, instance);
	for (i = 0; i < PPR_MAX_ATTRS; i++)
		ppr_ctx->ppr_attrs[i] = &ppr_ctx->ppr_dev_attr[i].dev_attr.attr;

	sprintf(ppr_ctx->name, "%s%d", "ppr", instance);
	group->name = ppr_ctx->name;
	group->attrs = ppr_ctx->ppr_attrs;
	group->is_visible  = ppr_attr_visible;

	attr_groups[0] = group;

	return 0;
}

/**
 * edac_ppr_get_desc - get edac PPR descriptors
 * @ppr_dev: client PPR device
 * @attr_groups: pointer to attrribute group container
 * @instance: device's PPR instance number.
 *
 * Returns 0 on success, error otherwise.
 */
int edac_ppr_get_desc(struct device *ppr_dev,
		      const struct attribute_group **attr_groups,
		      u8 instance)
{
	if (!ppr_dev || !attr_groups)
		return -EINVAL;

	return ppr_create_desc(ppr_dev, attr_groups, instance);
}
