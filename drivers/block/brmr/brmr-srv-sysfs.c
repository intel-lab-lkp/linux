// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Block device over RMR (BRMR)
 *
 * Copyright (c) 2026 IONOS SE
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/parser.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/slab.h>

#include "brmr-srv.h"
#include "rmr-srv.h"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

static struct class *rmr_str_class;
static struct device *rmr_ctl_dev;
static struct device *rmr_strs_dev;

enum {
	BRMR_SRV_STR_OPT_ERR = 0,
	BRMR_SRV_STR_OPT_DEVICE = 1 << 0,
	BRMR_SRV_STR_OPT_POOL = 1 << 2,
	BRMR_SRV_STR_OPT_MAPPED_SIZE = 1 << 3,
	BRMR_SRV_STR_OPT_MODE = 1 << 4,
};

static const unsigned int rmr_str_opt_mandatory[] = {
	BRMR_SRV_STR_OPT_POOL,
	BRMR_SRV_STR_OPT_DEVICE,
	BRMR_SRV_STR_OPT_MAPPED_SIZE,
};

static const match_table_t rmr_str_opt_tokens = {
	{ BRMR_SRV_STR_OPT_POOL, "pool=%s" },
	{ BRMR_SRV_STR_OPT_DEVICE, "device=%s" },
	{ BRMR_SRV_STR_OPT_MAPPED_SIZE, "mapped_size=%s" },
	{ BRMR_SRV_STR_OPT_MODE, "mode=%s" },
	{ BRMR_SRV_STR_OPT_ERR, NULL },
};

struct brmr_srv_str_options {
	char *pool;
	char *device;
	unsigned long mapped_size;
};

static void brmr_srv_remove_store(struct brmr_srv_blk_dev *dev, struct kobj_attribute *attr,
				  bool delete)
{
	mutex_lock(&store_mutex);

	blk_str_destroy_sysfs_files(dev, &attr->attr);

	brmr_srv_blk_close(dev, delete);

	pr_info("put blkdev %s\n", dev->bdev->bd_disk->disk_name);
	bdev_fput(dev->bdev_file);

	pr_info("%s store %s, store name %s.\n", (delete ? "Delete" : "Remove"),
		dev->name, dev->poolname);
	brmr_srv_blk_destroy(dev);
	mutex_unlock(&store_mutex);
}

static int brmr_srv_parse_add_opts(const char *buf, struct brmr_srv_str_options *opt,
				   unsigned int *replace)
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
	while ((p = strsep(&sep_opt, " ")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, rmr_str_opt_tokens, args);
		opt_mask |= token;

		switch (token) {
		case BRMR_SRV_STR_OPT_POOL:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(p) > NAME_MAX) {
				pr_err("add_store: pool name is too long\n");
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			strscpy(opt->pool, p, NAME_MAX);
			kfree(p);
			break;

		case BRMR_SRV_STR_OPT_DEVICE:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}
			if (strlen(p) > NAME_MAX) {
				pr_err("add_store: device name is too long\n");
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			strscpy(opt->device, p, NAME_MAX);
			kfree(p);
			break;

		case BRMR_SRV_STR_OPT_MAPPED_SIZE:
			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}

			ret = kstrtoul(p, 0, &opt->mapped_size);
			if (ret) {
				pr_err("mapped_size isn't an integer: %d\n", ret);
				kfree(p);
				goto out;
			}

			if (opt->mapped_size == 0) {
				pr_err("mapped_size cannot be 0\n");
				ret = -EINVAL;
				kfree(p);
				goto out;
			}

			kfree(p);
			break;

		case BRMR_SRV_STR_OPT_MODE:
			if (!replace) {
				pr_err("%s: mode option not supported here\n", __func__);
				ret = -EINVAL;
				goto out;
			}

			p = match_strdup(args);
			if (!p) {
				ret = -ENOMEM;
				goto out;
			}

			if (!strcmp(p, "replace")) {
				*replace = true;
			} else {
				pr_err("%s: Unknown mode '%s'\n", __func__, p);
				ret = -EINVAL;
				kfree(p);
				goto out;
			}
			kfree(p);
			break;

		default:
			pr_err("add_store: Unknown parameter or missing value '%s'\n",
			       p);
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < ARRAY_SIZE(rmr_str_opt_mandatory); i++) {
		if ((opt_mask & rmr_str_opt_mandatory[i])) {
			ret = 0;
		} else {
			pr_err("add_store: Parameters missing\n");
			ret = -EINVAL;
			break;
		}
	}

out:
	kfree(options);
	return ret;
}

static ssize_t blk_str_dev_size_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *page)
{
	struct brmr_srv_blk_dev *dev;

	dev = container_of(kobj, struct brmr_srv_blk_dev, kobj);

	return sysfs_emit(page, "%llu\n", dev->dev_size);
}

static struct kobj_attribute blk_str_dev_size_attr =
	__ATTR(dev_size, 0644, blk_str_dev_size_show, NULL);

static ssize_t blk_str_mapped_size_show(struct kobject *kobj,
					struct kobj_attribute *attr, char *page)
{
	struct brmr_srv_blk_dev *dev;

	dev = container_of(kobj, struct brmr_srv_blk_dev, kobj);

	return sysfs_emit(page, "%llu\n", dev->mapped_size);
}

static struct kobj_attribute blk_str_mapped_size_attr =
	__ATTR(mapped_size, 0644, blk_str_mapped_size_show, NULL);

static ssize_t blk_str_bdev_name_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *page)
{
	struct brmr_srv_blk_dev *dev;

	dev = container_of(kobj, struct brmr_srv_blk_dev, kobj);

	return sysfs_emit(page, "%s\n", dev->name);
}

static struct kobj_attribute blk_str_bdev_name_attr =
	__ATTR(bdev_name, 0644, blk_str_bdev_name_show, NULL);

static ssize_t blk_str_remove_store_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 to remove the store\n");
}

static ssize_t blk_str_remove_store_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	struct brmr_srv_blk_dev *dev;

	dev = container_of(kobj, struct brmr_srv_blk_dev, kobj);
	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       dev->name, attr->attr.name, buf);
		return -EINVAL;
	}

	brmr_srv_remove_store(dev, attr, false);

	return count;
}

static struct kobj_attribute blk_str_remove_store_attr =
	__ATTR(remove_store, 0644,
	       blk_str_remove_store_show, blk_str_remove_store_store);

static ssize_t blk_str_delete_store_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *page)
{
	return scnprintf(page, PAGE_SIZE, "Usage: echo 1 to delete the store\n");
}

static ssize_t blk_str_delete_store_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	struct brmr_srv_blk_dev *dev;

	dev = container_of(kobj, struct brmr_srv_blk_dev, kobj);
	if (!sysfs_streq(buf, "1")) {
		pr_err("%s, %s unknown value: '%s'\n",
		       dev->name, attr->attr.name, buf);
		return -EINVAL;
	}

	brmr_srv_remove_store(dev, attr, true);

	return count;
}

static struct kobj_attribute blk_str_delete_store_attr =
	__ATTR(delete_store, 0644,
	       blk_str_delete_store_show, blk_str_delete_store_store);

static ssize_t state_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *page)
{
	struct brmr_srv_blk_dev *dev;
	size_t count = 0;

	dev = container_of(kobj, struct brmr_srv_blk_dev, kobj);

	if (test_bit(BRMR_SRV_STORE_OPEN, &dev->state))
		count += sysfs_emit_at(page, count, "open\n");
	else
		count += sysfs_emit_at(page, count, "closed\n");

	if (test_bit(BRMR_SRV_STORE_MAPPED, &dev->state))
		count += sysfs_emit_at(page, count, "mapped\n");
	else
		count += sysfs_emit_at(page, count, "unmapped\n");

	return count;
}

static struct kobj_attribute blk_str_state_attr =
	__ATTR_RO(state);

static struct attribute *blk_str_map_attrs[] = {
	&blk_str_dev_size_attr.attr,
	&blk_str_mapped_size_attr.attr,
	&blk_str_bdev_name_attr.attr,
	&blk_str_remove_store_attr.attr,
	&blk_str_delete_store_attr.attr,
	&blk_str_state_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(blk_str_map);

static struct kobj_type blk_str_device_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = blk_str_map_groups,
};

static int blk_str_create_sysfs_files(struct brmr_srv_blk_dev *dev)
{
	int ret;

	ret = kobject_init_and_add(&dev->kobj, &blk_str_device_ktype,
				   &rmr_strs_dev->kobj,
				   "%s", dev->poolname);
	if (ret)
		pr_err("Failed to create sysfs dir for store %s, name %s, err=%d\n",
		       dev->name, dev->poolname, ret);

	return ret;
}

void blk_str_destroy_sysfs_files(struct brmr_srv_blk_dev *dev,
				 const struct attribute *sysfs_self)
{
	if (sysfs_self)
		sysfs_remove_file_self(&dev->kobj, sysfs_self);

	kobject_del(&dev->kobj);
	kobject_put(&dev->kobj);
}

/**
 * brmr_srv_blk_dev_exit() - Destroy and put the blkdev
 *
 * @dev:	RMR block device structure to be used.
 *
 * Description:
 *	This function gives up the blkdev reference, and destroys the rmr block device
 */
static void brmr_srv_blk_dev_exit(struct brmr_srv_blk_dev *dev)
{
	pr_info("%s: put blkdev %s\n", __func__, dev->name);
	bdev_fput(dev->bdev_file);

	brmr_srv_blk_destroy(dev);
}

/**
 * brmr_srv_blk_dev_init() - Create and initialize a brmr server store block device
 *
 * @pool_name:	Name to be given to the created rmr block device
 * @dev_name:	path to the block device
 * @mapped_size:mapped size of the block device
 *
 * Description:
 *	This function checks whether the rmr pool is available to be registered.
 *	It then creates the block device, and initializes it.
 *
 * Return:
 *	Pointer to the created rmr block device on success
 *	Error pointer on error
 */
static struct brmr_srv_blk_dev *brmr_srv_blk_dev_init(char *pool_name, char *dev_name,
						      u64 mapped_size)
{
	struct file *bdev_file;
	struct brmr_srv_blk_dev *dev;

	dev = brmr_srv_blk_create(dev_name, pool_name);
	if (IS_ERR(dev)) {
		pr_err("failed to alloc store for device %s: %pe\n", pool_name, dev);
		return dev;
	}

	bdev_file = bdev_file_open_by_path(dev_name, DEFAULT_BLK_OPEN_FLAGS,
					   dev, NULL);
	if (IS_ERR(bdev_file)) {
		pr_err("%s: bdev_file_open_by_path for device %s failed with err (%pe)\n",
		       __func__, dev_name, bdev_file);
		brmr_srv_blk_destroy(dev);
		return ERR_CAST(bdev_file);
	}

	dev->bdev_file = bdev_file;
	dev->bdev = file_bdev(bdev_file);
	dev->dev_size = get_capacity(dev->bdev->bd_disk);
	strscpy(dev->name, dev->bdev->bd_disk->disk_name, sizeof(dev->name));

	if (mapped_size < BLK_STR_MIN_MAPPED_SIZE) {
		pr_err("%s: Given mapped size %llu less than minimum default(%lu) for dev %s\n",
		       __func__, mapped_size, BLK_STR_MIN_MAPPED_SIZE, dev->name);
		brmr_srv_blk_dev_exit(dev);
		return ERR_PTR(-ENOSPC);
	}

	if (mapped_size + BLK_STR_MD_SIZE_SECTORS > dev->dev_size) {
		pr_err("can not map %llu, only %llu available %s\n",
		       mapped_size, dev->dev_size - BLK_STR_MD_SIZE_SECTORS, dev->name);
		brmr_srv_blk_dev_exit(dev);
		return ERR_PTR(-ENOSPC);
	}

	dev->mapped_size = mapped_size;

	pr_info("%s: succeeded\n", __func__);

	return dev;
}

static ssize_t brmr_srv_create_store_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct brmr_srv_str_options opt;
	char dev_name[NAME_MAX];
	char pool_name[NAME_MAX];
	struct brmr_srv_blk_dev *dev;
	struct brmr_srv_blk_dev_meta *md_page;
	int md_state, err;

	opt.pool = pool_name;
	opt.device = dev_name;
	opt.mapped_size = 0;

	if (brmr_srv_parse_add_opts(buf, &opt, NULL))
		goto out;

	md_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!md_page) {
		pr_err("%s: Failed to allocate page to read md\n", __func__);
		goto out;
	}

	mutex_lock(&store_mutex);

	dev = brmr_srv_blk_dev_init(pool_name, dev_name, opt.mapped_size);
	if (IS_ERR(dev)) {
		pr_err("%s: brmr_srv_blk_dev_init failed: %pe\n", __func__, dev);
		goto mut_unlock;
	}

	md_state = brmr_srv_read_and_check_md(dev, md_page);
	if (md_state != -1) {
		/*
		 * read and check md failed. It could be read error or that md exists
		 */
		pr_err("%s: md read and check failed: %d\n", __func__, md_state);
		goto dev_exit;
	}

	err = brmr_srv_blk_open(dev, dev_name, true, false);
	if (err) {
		pr_err("failed to open %s, err %d\n", dev_name, err);
		goto dev_exit;
	}

	err = blk_str_create_sysfs_files(dev);
	if (err) {
		pr_err("failed to create sysfs files\n");
		goto dev_close;
	}

	mutex_unlock(&store_mutex);
	pr_info("Created new blk store for %s, with disk %s\n", pool_name, dev_name);

	kfree(md_page);
	return count;

dev_close:
	brmr_srv_blk_close(dev, true);
dev_exit:
	brmr_srv_blk_dev_exit(dev);
mut_unlock:
	mutex_unlock(&store_mutex);
	kfree(md_page);
out:
	return -EINVAL;
}

static ssize_t brmr_srv_create_store_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *page)
{
	return scnprintf(page, PAGE_SIZE,
			 "Usage: echo \"pool=<name of the rmr pool> device=<full path of block device> mapped_size=<size of given block device to be mapped>\" > %s\n\n",
			 attr->attr.name);
}

static struct kobj_attribute brmr_srv_create_store_attr =
	__ATTR(create_store, 0644,
	       brmr_srv_create_store_show, brmr_srv_create_store_store);

static ssize_t brmr_srv_add_store_store(struct kobject *kobj, struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct brmr_srv_blk_dev *dev;
	char dev_name[NAME_MAX];
	char pool_name[NAME_MAX];
	struct brmr_srv_str_options opt;
	struct brmr_srv_blk_dev_meta *md_page;
	int md_state, ret;
	unsigned int replace = false;

	opt.pool = pool_name;
	opt.device = dev_name;
	opt.mapped_size = 0;

	if (brmr_srv_parse_add_opts(buf, &opt, &replace))
		goto out;

	/*
	 * Disable replace mode for now.
	 * Most of the code for replace mode to work is present, but there are some
	 * edge cases which needs work, and a info exchange between storage nodes which
	 * needs to be added.
	 */
	if (replace) {
		pr_err("%s: Replace mode not supported yet\n", __func__);
		goto out;
	}

	md_page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!md_page) {
		pr_err("Failed to allocate page to read md\n");
		goto out;
	}

	mutex_lock(&store_mutex);

	dev = brmr_srv_blk_dev_init(pool_name, dev_name, opt.mapped_size);
	if (IS_ERR(dev)) {
		pr_err("brmr_srv_blk_dev_init failed: %pe\n", dev);
		goto mut_unlock;
	}

	md_state = brmr_srv_read_and_check_md(dev, md_page);
	if (md_state == -1) {
		/*
		 * md doesn't exists. This means the disk is an empty one.
		 * We have to replace, so check the mode first
		 */
		if (!replace) {
			pr_err("%s: Incorrect mode %d. md doesn't exists\n", __func__, replace);
			goto dev_exit;
		}

		/*
		 * we have to do the following,
		 *
		 * 1) Check params like mapped size from at least one other storage node
		 * 2) Do discard
		 */
		pr_info("%s: No md found. Replacing disk %s for pool %s, size %llu\n",
			__func__, dev_name, pool_name, dev->mapped_size);
	} else if (md_state == 0) {
		/*
		 * md exists.
		 * We are restoring an earlier used device.
		 */
		if (replace) {
			pr_err("%s: Incorrect mode %d. md exists\n", __func__, replace);
			goto dev_exit;
		}

		/*
		 * Validate the metadata stored with the data provided.
		 */
		ret = brmr_srv_blk_validate_md(dev, md_page);
		if (ret) {
			pr_err("Local metadata validation failed\n");
			goto dev_exit;
		}

		memcpy(&dev->dev_params, &md_page->dev_params, sizeof(struct rmr_blk_dev_params));
		dev->state = md_page->state;

		pr_info("%s: md found. Re-adding disk %s for pool %s, size %llu\n",
			__func__, dev_name, pool_name, dev->mapped_size);
	} else {
		pr_err("%s: md cannot be read for block device %s, Err = %d\n",
		       __func__, dev->name, md_state);
		goto dev_exit;
	}

	if (brmr_srv_blk_open(dev, dev_name, false /* create */, replace)) {
		pr_err("failed to open %s\n", dev_name);
		goto dev_exit;
	}

	ret = blk_str_create_sysfs_files(dev);
	if (ret) {
		pr_err("failed to create sysfs files\n");
		goto dev_close;
	}

	mutex_unlock(&store_mutex);

	kfree(md_page);
	return count;

dev_close:
	brmr_srv_blk_close(dev, replace);
dev_exit:
	brmr_srv_blk_dev_exit(dev);
mut_unlock:
	mutex_unlock(&store_mutex);
	kfree(md_page);
out:
	return -EINVAL;
}

static ssize_t brmr_srv_add_store_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *page)
{
	return scnprintf(page, PAGE_SIZE,
			 "Usage: echo \"pool=<name of the rmr pool> device=<full path of block device> mapped_size=<size of given block device to be mapped>\" > %s\n\n",
			 attr->attr.name);
}

static struct kobj_attribute brmr_srv_add_store_attr =
	__ATTR(add_store, 0644,
	       brmr_srv_add_store_show, brmr_srv_add_store_store);

static struct attribute *default_attrs[] = {
	&brmr_srv_create_store_attr.attr,
	&brmr_srv_add_store_attr.attr,
	NULL,
};

static struct attribute_group default_attr_group = {
	.attrs = default_attrs,
};

int brmr_srv_create_sysfs_files(void)
{
	int err;
	dev_t devt = MKDEV(0, 0);

	rmr_str_class = class_create("brmr-server");
	if (IS_ERR(rmr_str_class))
		return PTR_ERR(rmr_str_class);

	rmr_ctl_dev = device_create(rmr_str_class, NULL, devt, NULL, "ctl");
	if (IS_ERR(rmr_ctl_dev)) {
		err = PTR_ERR(rmr_ctl_dev);
		goto cls_destroy;
	}

	rmr_strs_dev = device_create(rmr_str_class, NULL, devt, NULL, "stores");
	if (IS_ERR(rmr_strs_dev)) {
		err = PTR_ERR(rmr_strs_dev);
		goto ctl_destroy;
	}

	err = sysfs_create_group(&rmr_ctl_dev->kobj, &default_attr_group);
	if (unlikely(err))
		goto strs_destroy;

	return 0;

strs_destroy:
	device_unregister(rmr_strs_dev);
ctl_destroy:
	device_unregister(rmr_ctl_dev);
cls_destroy:
	class_destroy(rmr_str_class);

	return err;
}

void brmr_srv_destroy_sysfs_files(void)
{
	sysfs_remove_group(&rmr_ctl_dev->kobj, &default_attr_group);
	device_unregister(rmr_strs_dev);
	device_unregister(rmr_ctl_dev);
	class_destroy(rmr_str_class);
}
