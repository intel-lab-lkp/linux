// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/parser.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#include "brmr-clt.h"

static struct device *brmr_dev;
static struct class *brmr_dev_class;
static struct kobject *brmr_devs_kobj;

enum {
	BRMR_OPT_ERR		= 0,
	BRMR_OPT_POOL		= 1 << 1,
	BRMR_OPT_SIZE		= 1 << 2,
};

static int brmr_clt_create_dev_sysfs_files(struct brmr_clt_dev *dev);
static int brmr_add_dev_symlink(struct brmr_clt_dev *dev);

static unsigned int brmr_opt_mandatory[] = {
	BRMR_OPT_POOL,
};

static const match_table_t brmr_opt_tokens = {
	{	BRMR_OPT_POOL,		"pool=%s"	},
	{	BRMR_OPT_SIZE,		"size=%s"	},
	{	BRMR_OPT_ERR,		NULL		},
};

/* remove new line from string */
static void strip(char *s)
{
	char *p = s;

	while (*s != '\0') {
		if (*s != '\n')
			*p++ = *s++;
		else
			++s;
	}
	*p = '\0';
}

static int brmr_clt_parse_options(const char *buf,
			      char *pool,
			      unsigned long *size)
{
	char *options, *sep_opt;
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int opt_mask = 0;
	int token;
	int ret = -EINVAL;
	int i;

	options = kstrdup(buf, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	sep_opt = strstrip(options);
	strip(sep_opt);
	while ((p = strsep(&sep_opt, " ")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, brmr_opt_tokens, args);
		opt_mask |= token;

		switch (token) {
		case BRMR_OPT_POOL:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(p) > NAME_MAX) {
				pr_err("poolname too long\n");
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			strscpy(pool, p, NAME_MAX);
			kfree(p);
			break;

		case BRMR_OPT_SIZE:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}

			/*
			 * The conventional semantics are that if the number begins with 0x, it will
			 * be parsed as hexadecimal; if it begins with 0, it will be parsed as
			 * octal; otherwise, it will be parsed as decimal.
			 */
			ret = kstrtoul(p, 0, size);
			if (ret) {
				pr_err("size '%s' isn't an integer: %d\n", p, ret);
				kfree(p);
				goto out;
			}
			kfree(p);
			break;


		default:
			pr_err("unknown parameter or missing value"
			       " '%s'\n", p);
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < ARRAY_SIZE(brmr_opt_mandatory); i++) {
		if ((opt_mask & brmr_opt_mandatory[i])) {
			ret = 0;
		} else {
			pr_err("parameters missing\n");
			ret = -EINVAL;
			break;
		}
	}

out:
	kfree(options);
	return ret;
}

static ssize_t brmr_map_device_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo \""
			 "pool=<name of the RMR pool> "
			 "size=<size of the volume in sectors>\" > %s\n",
			 attr->attr.name);
}

static ssize_t brmr_map_device_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	struct brmr_clt_dev *dev;
	char pool[NAME_MAX];
	unsigned long size = 0;
	int ret;

	ret = brmr_clt_parse_options(buf, pool, &size);
	if (ret)
		goto err;

	dev = find_and_get_device(pool);
	if (dev) {
		pr_err("Device exists and opened as %s\n",
		       dev->gd->disk_name);
		brmr_clt_put_dev(dev);
		ret = -EEXIST;
		goto err;
	}

	dev = brmr_clt_map_device(pool, size);
	if (IS_ERR(dev)) {
		pr_err("Error mapping device to pool %s\n", pool);
		ret = PTR_ERR(dev);
		goto err;
	}
	ret = brmr_clt_create_dev_sysfs_files(dev);
	if (ret)
		goto close_device;

	ret = brmr_add_dev_symlink(dev);
	if (ret)
		goto destroy_sysfs;

	return count;

destroy_sysfs:
	sysfs_remove_link(&dev->kobj, BRMR_LINK_NAME);
	brmr_clt_destroy_dev_sysfs_files(dev, NULL);
close_device:
	brmr_clt_close_device(dev, NULL);
err:
	return ret;
}

static struct kobj_attribute brmr_map_device_attr =
	__ATTR(map_device, 0644,
	       brmr_map_device_show, brmr_map_device_store);

static struct attribute *default_attrs[] = {
	&brmr_map_device_attr.attr,
	NULL,
};

static struct attribute_group default_attr_group = {
	.attrs = default_attrs,
};

static ssize_t brmr_unmap_device_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo <normal|force> > %s\n",
			 attr->attr.name);
}

static ssize_t brmr_unmap_device_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct brmr_clt_dev *dev;
	int err;

	dev = container_of(kobj, struct brmr_clt_dev, kobj);

	if (!sysfs_streq(buf, "1")) {
		pr_err("%s: unknown value: '%s'\n", attr->attr.name, buf);
		return -EINVAL;
	}

	pr_info("Closing device %s.\n", dev->gd->disk_name);

	/*
	 * We take explicit module reference only for one reason: do not
	 * race with lockless ibnbd_destroy_sessions().
	 */
	if (!try_module_get(THIS_MODULE)) {
		return -ENODEV;
	}
	err = brmr_clt_close_device(dev, &attr->attr);
	if (unlikely(err)) {
		if (unlikely(err != -EALREADY))
			pr_err("unmap_device %s: %d\n",
			       dev->gd->disk_name, err);
		goto module_put;
	}

	/*
	 * Here device can be vanished!
	 */
	err = count;

module_put:
	module_put(THIS_MODULE);

	return err;
}

static struct kobj_attribute brmr_unmap_device_attr =
	__ATTR(unmap_device, 0644,
	       brmr_unmap_device_show, brmr_unmap_device_store);

static ssize_t brmr_clt_device_state_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *page)
{
	struct brmr_clt_dev *dev;
	int cnt;

	dev = container_of(kobj, struct brmr_clt_dev, kobj);

	switch (dev->dev_state) {
	case DEV_STATE_INIT:
		cnt = sysfs_emit(page, "init\n");
		break;
	case DEV_STATE_READY:
		cnt = sysfs_emit(page, "ready\n");
		break;
	case DEV_STATE_DISCONNECTED:
		cnt = sysfs_emit(page, "disconnected\n");
		break;
	case DEV_STATE_CLOSING:
		cnt = sysfs_emit(page, "closing\n");
		break;
	default:
		cnt = sysfs_emit(page, "unknown\n");
		break;
	}

	if (dev->map_incomplete)
		cnt += sysfs_emit_at(page, cnt, "degraded\n");

	return cnt;
}

static struct kobj_attribute brmr_clt_device_state =
	__ATTR(state, 0444, brmr_clt_device_state_show, NULL);

static struct attribute *brmr_clt_dev_attrs[] = {
	&brmr_unmap_device_attr.attr,
	&brmr_clt_device_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(brmr_clt_dev);

static struct kobj_type brmr_clt_device_ktype = {
	.sysfs_ops      = &kobj_sysfs_ops,
	.default_groups  = brmr_clt_dev_groups,
};

static struct kobj_type brmr_clt_stats_ktype = {
	.sysfs_ops      = &kobj_sysfs_ops,
};

static int brmr_clt_create_stats_files(struct kobject *kobj,
				   struct kobject *kobj_stats);

static int brmr_clt_create_dev_sysfs_files(struct brmr_clt_dev *dev)
{
	int ret;

	ret = kobject_init_and_add(&dev->kobj, &brmr_clt_device_ktype,
				   brmr_devs_kobj,
				   "%s", dev->gd->disk_name);
	if (ret)
		pr_err("Failed to create sysfs dir for device '%s': %d\n",
		       dev->gd->disk_name, ret);

	ret = brmr_clt_create_stats_files(&dev->kobj, &dev->kobj_stats);
	if (unlikely(ret)) {
		pr_err("Failed to create sysfs stats files "
		       "for device '%s': %d\n", dev->gd->disk_name, ret);
		kobject_del(&dev->kobj);
		kobject_put(&dev->kobj);
	}
	return ret;
}

static int brmr_add_dev_symlink(struct brmr_clt_dev *dev)
{
	struct kobject *gd_kobj = &disk_to_dev(dev->gd)->kobj;
	int ret;

	ret = sysfs_create_link(&dev->kobj, gd_kobj, BRMR_LINK_NAME);
	if (ret) {
		pr_err("Creating symlink for %s failed, err: %d\n",
		       dev->gd->disk_name, ret);
	}

	return ret;
}

void brmr_clt_destroy_dev_sysfs_files(struct brmr_clt_dev *dev,
				     const struct attribute *sysfs_self)
{
	if (dev->kobj.state_in_sysfs) {

		kobject_del(&dev->kobj_stats);
		kobject_put(&dev->kobj_stats);
		if (sysfs_self)
			sysfs_remove_file_self(&dev->kobj, sysfs_self);
		kobject_del(&dev->kobj);
		kobject_put(&dev->kobj);
	}
}

int brmr_clt_create_sysfs_files(void)
{
	int err;

	brmr_dev_class = class_create("brmr-client");
	if (IS_ERR(brmr_dev_class))
		return PTR_ERR(brmr_dev_class);

	brmr_dev = device_create(brmr_dev_class, NULL,
				 MKDEV(0, 0), NULL, "ctl");
	if (IS_ERR(brmr_dev)) {
		err = PTR_ERR(brmr_dev);
		goto cls_destroy;
	}
	brmr_devs_kobj = kobject_create_and_add("devices", &brmr_dev->kobj);
	if (unlikely(!brmr_devs_kobj)) {
		err = -ENOMEM;
		goto dev_destroy;
	}
	err = sysfs_create_group(&brmr_dev->kobj, &default_attr_group);
	if (unlikely(err))
		goto put_devs_kobj;

	return 0;

put_devs_kobj:
	kobject_del(brmr_devs_kobj);
	kobject_put(brmr_devs_kobj);
dev_destroy:
	device_unregister(brmr_dev);
cls_destroy:
	class_destroy(brmr_dev_class);

	return err;
}

void brmr_clt_destroy_sysfs_files(void)
{
	sysfs_remove_group(&brmr_dev->kobj, &default_attr_group);
	kobject_del(brmr_devs_kobj);
	kobject_put(brmr_devs_kobj);
	device_unregister(brmr_dev);
	class_destroy(brmr_dev_class);
}

STAT_ATTR(struct brmr_clt_dev, requests,
	  brmr_clt_stats_rq_to_str, brmr_clt_reset_submitted_req);
STAT_ATTR(struct brmr_clt_dev, request_sizes,
	  brmr_clt_stats_sizes_to_str, brmr_clt_reset_req_sizes);
STAT_ATTR(struct brmr_clt_dev, sts_resource,
	  brmr_stats_sts_resource_to_str, brmr_clt_reset_sts_resource);
STAT_ATTR(struct brmr_clt_dev, sts_resource_per_cpu,
	  brmr_stats_sts_resource_per_cpu_to_str, brmr_clt_reset_sts_resource);

static struct attribute *brmr_stats_attrs[] = {
	&requests_attr.attr,
	&request_sizes_attr.attr,
	&sts_resource_attr.attr,
	&sts_resource_per_cpu_attr.attr,
	NULL,
};

static struct attribute_group brmr_stats_attr_group = {
	.attrs = brmr_stats_attrs,
};

static int brmr_clt_create_stats_files(struct kobject *kobj,
				   struct kobject *kobj_stats)
{
	int ret;

	ret = kobject_init_and_add(kobj_stats, &brmr_clt_stats_ktype, kobj, "stats");
	if (ret) {
		pr_err("Failed to init and add stats kobject, err: %d\n",
		       ret);
		return ret;
	}

	ret = sysfs_create_group(kobj_stats, &brmr_stats_attr_group);
	if (ret) {
		pr_err("failed to create stats sysfs group, err: %d\n",
		       ret);
		goto put_stats_obj;
	}

	return 0;

put_stats_obj:
	kobject_del(kobj_stats);
	kobject_put(kobj_stats);

	return ret;
}
