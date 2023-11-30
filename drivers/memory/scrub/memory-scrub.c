// SPDX-License-Identifier: GPL-2.0
/*
 * Memory scrub controller driver support to configure
 * the parameters of the memory scrubbers and enable.
 *
 * Copyright (c) 2023 HiSilicon Limited.
 */

#define pr_fmt(fmt)     "MEM SCRUB: " fmt

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <memory/memory-scrub.h>

/* memory scrubber config definitions */
#define SCRUB_ID_PREFIX "scrub"
#define SCRUB_ID_FORMAT SCRUB_ID_PREFIX "%d"
#define SCRUB_DEV_MAX_NAME_LENGTH	128

static DEFINE_IDA(scrub_ida);

struct scrub_device {
	char name[SCRUB_DEV_MAX_NAME_LENGTH];
	int id;
	struct device dev;
	const struct scrub_source_info *source_info;
	struct list_head tzdata;
	char (*region_name)[];
	struct attribute_group group;
	int ngroups;
	struct attribute_group *region_groups;
	const struct attribute_group **groups;
};

#define to_scrub_device(d) container_of(d, struct scrub_device, dev)
#define SCRUB_MAX_SYSFS_ATTR_NAME_LENGTH	64

struct scrub_device_attribute {
	struct device_attribute dev_attr;
	const struct scrub_ops *ops;
	u32 attr;
	int region_id;
	char name[SCRUB_MAX_SYSFS_ATTR_NAME_LENGTH];
};

#define to_scrub_attr(d) \
	container_of(d, struct scrub_device_attribute, dev_attr)
#define to_dev_attr(a) container_of(a, struct device_attribute, attr)

static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_scrub_device(dev)->name);
}
static DEVICE_ATTR_RO(name);

static struct attribute *scrub_dev_attrs[] = {
	&dev_attr_name.attr,
	NULL
};

static umode_t scrub_dev_attr_is_visible(struct kobject *kobj,
					    struct attribute *attr, int n)
{
	if (attr != &dev_attr_name.attr)
		return 0;

	return attr->mode;
}

static const struct attribute_group scrub_dev_attr_group = {
	.attrs		= scrub_dev_attrs,
	.is_visible	= scrub_dev_attr_is_visible,
};

static const struct attribute_group *scrub_dev_attr_groups[] = {
	&scrub_dev_attr_group,
	NULL
};

static void scrub_free_attrs(struct attribute **attrs)
{
	int i;

	for (i = 0; attrs[i]; i++) {
		struct device_attribute *dattr = to_dev_attr(attrs[i]);
		struct scrub_device_attribute *hattr = to_scrub_attr(dattr);

		kfree(hattr);
	}
	kfree(attrs);
}

static void scrub_dev_release(struct device *dev)
{
	int count;
	struct attribute_group *group;
	struct scrub_device *scrub_dev = to_scrub_device(dev);

	for (count = 0; count < scrub_dev->ngroups; count++) {
		group = (struct attribute_group *)scrub_dev->groups[count];
		if (group)
			scrub_free_attrs(group->attrs);
	}
	kfree(scrub_dev->region_name);
	kfree(scrub_dev->region_groups);
	kfree(scrub_dev->groups);
	ida_free(&scrub_ida, scrub_dev->id);
	kfree(scrub_dev);
}

static struct class scrub_class = {
	.name = "scrub",
	.dev_groups = scrub_dev_attr_groups,
	.dev_release = scrub_dev_release,
};

/* sysfs attribute management */

static ssize_t scrub_attr_show(struct device *dev,
			       struct device_attribute *devattr, char *buf)
{
	int ret;
	u64 val;
	struct scrub_device_attribute *hattr = to_scrub_attr(devattr);

	ret = hattr->ops->read(dev, hattr->attr, hattr->region_id, &val);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%lld\n", val);
}

static ssize_t scrub_attr_show_hex(struct device *dev,
				   struct device_attribute *devattr, char *buf)
{
	int ret;
	u64 val;
	struct scrub_device_attribute *hattr = to_scrub_attr(devattr);

	ret = hattr->ops->read(dev, hattr->attr, hattr->region_id, &val);
	if (ret < 0)
		return ret;

	return sprintf(buf, "0x%llx\n", val);
}

static ssize_t scrub_attr_show_string(struct device *dev,
				      struct device_attribute *devattr,
				      char *buf)
{
	int ret;
	struct scrub_device_attribute *hattr = to_scrub_attr(devattr);

	ret = hattr->ops->read_string(dev, hattr->attr, hattr->region_id, buf);
	if (ret < 0)
		return ret;

	return strlen(buf);
}

static ssize_t scrub_attr_store(struct device *dev,
				struct device_attribute *devattr,
				const char *buf, size_t count)
{
	int ret;
	long val;
	struct scrub_device_attribute *hattr = to_scrub_attr(devattr);

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	ret = hattr->ops->write(dev, hattr->attr, hattr->region_id, val);
	if (ret < 0)
		return ret;

	return count;
}

static ssize_t scrub_attr_store_hex(struct device *dev,
				    struct device_attribute *devattr,
				    const char *buf, size_t count)
{
	int ret;
	u64 val;
	struct scrub_device_attribute *hattr = to_scrub_attr(devattr);

	ret = kstrtou64(buf, 16, &val);
	if (ret < 0)
		return ret;

	ret = hattr->ops->write(dev, hattr->attr, hattr->region_id, val);
	if (ret < 0)
		return ret;

	return count;
}

static bool is_hex_attr(u32 attr)
{
	return	(attr == scrub_addr_base) ||
		(attr == scrub_addr_size);
}

static bool is_string_attr(u32 attr)
{
	return	attr == scrub_speed_available ||
		attr == scrub_threshold_available;
}

static struct attribute *scrub_genattr(const void *drvdata,
				       u32 attr,
				       const char *attrb_name,
				       const struct scrub_ops *ops,
				       int region_id)
{
	umode_t mode;
	struct attribute *a;
	struct device_attribute *dattr;
	bool is_hex = is_hex_attr(attr);
	struct scrub_device_attribute *hattr;
	bool is_string = is_string_attr(attr);

	/* The attribute is invisible if there is no template string */
	if (!attrb_name)
		return ERR_PTR(-ENOENT);

	mode = ops->is_visible(drvdata, attr, region_id);
	if (!mode)
		return ERR_PTR(-ENOENT);

	if ((mode & 0444) && ((is_string && !ops->read_string) ||
			      (!is_string && !ops->read)))
		return ERR_PTR(-EINVAL);
	if ((mode & 0222) && (!ops->write))
		return ERR_PTR(-EINVAL);

	hattr = kzalloc(sizeof(*hattr), GFP_KERNEL);
	if (!hattr)
		return ERR_PTR(-ENOMEM);

	hattr->attr = attr;
	hattr->ops = ops;
	hattr->region_id = region_id;

	dattr = &hattr->dev_attr;
	if (is_string) {
		dattr->show = scrub_attr_show_string;
	} else {
		dattr->show = is_hex ? scrub_attr_show_hex : scrub_attr_show;
		dattr->store = is_hex ? scrub_attr_store_hex : scrub_attr_store;
	}

	a = &dattr->attr;
	sysfs_attr_init(a);
	a->name = attrb_name;
	a->mode = mode;

	return a;
}

static const char * const scrub_common_attrs[] = {
	/* scrub attributes - common */
	[scrub_addr_base] = "addr_base",
	[scrub_addr_size] = "addr_size",
	[scrub_enable] = "enable",
	[scrub_speed] = "speed",
	[scrub_speed_available] = "speed_available",
	/* scrub attributes - DDR5 ECS/common */
	[scrub_ecs_log_entry_type] = "ecs_log_entry_type",
	[scrub_ecs_log_entry_type_per_dram] = "ecs_log_entry_type_per_dram",
	[scrub_ecs_log_entry_type_per_memory_media] = "ecs_log_entry_type_per_memory_media",
	[scrub_mode] = "mode",
	[scrub_mode_counts_rows] = "mode_counts_rows",
	[scrub_mode_counts_codewords] = "mode_counts_codewords",
	[scrub_reset_counter] = "reset_counter",
	[scrub_threshold] = "threshold",
	[scrub_threshold_available] = "threshold_available",
};

static struct attribute **
scrub_create_attrs(const void *drvdata, const struct scrub_ops *ops, int region_id)
{
	u32 attr;
	int aindex = 0;
	struct attribute *a;
	struct attribute **attrs;

	attrs = kcalloc(max_attrs, sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return ERR_PTR(-ENOMEM);

	for (attr = 0; attr < max_attrs; attr++) {
		a = scrub_genattr(drvdata, attr, scrub_common_attrs[attr],
				  ops, region_id);
		if (IS_ERR(a)) {
			if (PTR_ERR(a) != -ENOENT) {
				scrub_free_attrs(attrs);
				return ERR_PTR(PTR_ERR(a));
			}
			continue;
		}
		attrs[aindex++] = a;
	}

	return attrs;
}

static struct device *
scrub_device_register(struct device *dev, const char *name, void *drvdata,
		      const struct scrub_ops *ops,
		      int nregions)
{
	struct device *hdev;
	struct attribute **attrs;
	int err, count, region_id;
	struct attribute_group *group;
	struct scrub_device *scrub_dev;
	char (*region_name)[SCRUB_MAX_SYSFS_ATTR_NAME_LENGTH];

	scrub_dev = kzalloc(sizeof(*scrub_dev), GFP_KERNEL);
	if (!scrub_dev)
		return ERR_PTR(-ENOMEM);
	hdev = &scrub_dev->dev;

	scrub_dev->id = ida_alloc(&scrub_ida, GFP_KERNEL);
	if (scrub_dev->id < 0) {
		err = -ENOMEM;
		goto free_scrub_dev;
	}
	int ngroups = 2; /* terminating NULL plus &scrub_dev->groups */

	ngroups += nregions;

	scrub_dev->groups = kcalloc(ngroups, sizeof(struct attribute_group *), GFP_KERNEL);
	if (!scrub_dev->groups) {
		err = -ENOMEM;
		goto free_ida;
	}

	if (nregions) {
		scrub_dev->region_groups = kcalloc(nregions, sizeof(struct attribute_group),
						   GFP_KERNEL);
		if (!scrub_dev->groups) {
			err = -ENOMEM;
			goto free_groups;
		}
		scrub_dev->region_name = kcalloc(nregions, SCRUB_MAX_SYSFS_ATTR_NAME_LENGTH,
						 GFP_KERNEL);
		if (!scrub_dev->region_name) {
			err = -ENOMEM;
			goto free_region_groups;
		}
	}

	ngroups = 0;
	scrub_dev->ngroups = 0;
	if (nregions) {
		region_name = scrub_dev->region_name;
		for (region_id = 0; region_id < nregions; region_id++) {
			attrs = scrub_create_attrs(drvdata, ops, region_id);
			if (IS_ERR(attrs)) {
				err = PTR_ERR(attrs);
				goto free_attrs;
			}
			snprintf((char *)region_name, SCRUB_MAX_SYSFS_ATTR_NAME_LENGTH,
				 "region%d", region_id);
			scrub_dev->region_groups[region_id].name = (char *)region_name;
			scrub_dev->region_groups[region_id].attrs = attrs;
			region_name++;
			scrub_dev->groups[ngroups++] = &scrub_dev->region_groups[region_id];
			scrub_dev->ngroups = ngroups;
		}
	} else {
		attrs = scrub_create_attrs(drvdata, ops, -1);
		if (IS_ERR(attrs)) {
			err = PTR_ERR(attrs);
			goto free_region_name;
		}
		scrub_dev->group.attrs = attrs;
		scrub_dev->groups[ngroups++] = &scrub_dev->group;
		scrub_dev->ngroups = ngroups;
	}

	hdev->groups = scrub_dev->groups;
	hdev->class = &scrub_class;
	hdev->parent = dev;
	dev_set_drvdata(hdev, drvdata);
	dev_set_name(hdev, SCRUB_ID_FORMAT, scrub_dev->id);
	snprintf(scrub_dev->name, SCRUB_DEV_MAX_NAME_LENGTH, "%s", name);
	err = device_register(hdev);
	if (err) {
		put_device(hdev);
		return ERR_PTR(err);
	}

	return hdev;

free_attrs:
	for (count = 0; count < scrub_dev->ngroups; count++) {
		group = (struct attribute_group *)scrub_dev->groups[count];
		if (group)
			scrub_free_attrs(group->attrs);
	}

free_region_name:
	kfree(scrub_dev->region_name);

free_region_groups:
	kfree(scrub_dev->region_groups);

free_groups:
	kfree(scrub_dev->groups);

free_ida:
	ida_free(&scrub_ida, scrub_dev->id);

free_scrub_dev:
	kfree(scrub_dev);
	return ERR_PTR(err);
}

static void devm_scrub_release(void *dev)
{
	struct device *hdev = dev;

	device_unregister(hdev);
}

/**
 * devm_scrub_device_register - register hw scrubber device
 * @dev: the parent device (mandatory)
 * @name: hw scrubber name attribute (mandatory)
 * @drvdata: driver data to attach to created device (mandatory)
 * @ops: pointer to scrub_ops structure (mandatory)
 * @nregions: number of scrub regions to create (optional)
 *
 * Returns the pointer to the new device. The new device is automatically
 * unregistered with the parent device.
 */
struct device *
devm_scrub_device_register(struct device *dev, const char *name,
			   void *drvdata,
			   const struct scrub_ops *ops,
			   int nregions)
{
	struct device *hdev;
	int ret;

	if (!dev || !name || !ops)
		return ERR_PTR(-EINVAL);

	hdev = scrub_device_register(dev, name, drvdata, ops, nregions);
	if (IS_ERR(hdev))
		return hdev;

	ret = devm_add_action_or_reset(dev, devm_scrub_release, hdev);
	if (ret)
		return ERR_PTR(ret);

	return hdev;
}
EXPORT_SYMBOL_GPL(devm_scrub_device_register);

static int __init memory_scrub_control_init(void)
{
	int err;

	err = class_register(&scrub_class);
	if (err) {
		pr_err("couldn't register memory scrub control sysfs class\n");
		return err;
	}

	return 0;
}
subsys_initcall(memory_scrub_control_init);
