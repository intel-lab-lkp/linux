// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Region - Support for FPGA programming under Linux
 *
 *  Copyright (C) 2013-2016 Altera Corporation
 *  Copyright (C) 2017 Intel Corporation
 */
#include <linux/fpga/fpga-bridge.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fpga/fpga-region.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

static DEFINE_IDA(fpga_region_ida);
static const struct class fpga_region_class;

struct fpga_region *
fpga_region_class_find(struct device *start, const void *data,
		       int (*match)(struct device *, const void *))
{
	struct device *dev;

	dev = class_find_device(&fpga_region_class, start, data, match);
	if (!dev)
		return NULL;

	return to_fpga_region(dev);
}
EXPORT_SYMBOL_GPL(fpga_region_class_find);

/**
 * fpga_region_get - get an exclusive reference to an fpga region
 * @region: FPGA Region struct
 *
 * Caller should call fpga_region_put() when done with region.
 *
 * Return:
 * * fpga_region struct if successful.
 * * -EBUSY if someone already has a reference to the region.
 * * -ENODEV if can't take parent driver module refcount.
 */
static struct fpga_region *fpga_region_get(struct fpga_region *region)
{
	struct device *dev = &region->dev;

	if (!mutex_trylock(&region->mutex)) {
		dev_dbg(dev, "%s: FPGA Region already in use\n", __func__);
		return ERR_PTR(-EBUSY);
	}

	get_device(dev);
	if (!try_module_get(region->ops_owner)) {
		put_device(dev);
		mutex_unlock(&region->mutex);
		return ERR_PTR(-ENODEV);
	}

	dev_dbg(dev, "get\n");

	return region;
}

/**
 * fpga_region_put - release a reference to a region
 *
 * @region: FPGA region
 */
static void fpga_region_put(struct fpga_region *region)
{
	struct device *dev = &region->dev;

	dev_dbg(dev, "put\n");

	module_put(region->ops_owner);
	put_device(dev);
	mutex_unlock(&region->mutex);
}

/**
 * fpga_region_add_variant - add a new variant to the FPGA region
 * @region: FPGA region
 * @name: string identifier for the variant
 * @is_enabled: whether this variant is the currently enabled variant
 * @priv: private data to be associated with the variant
 *
 * Allocates a new variant and adds it to the region's variant list.
 * If @is_enabled is true, this variant is marked as the enabled variant.
 *
 * Return: 0 on success, or -ENOMEM on memory allocation failure.
 */
int fpga_region_add_variant(struct fpga_region *region, const char *name,
			    bool is_enabled, void *priv)
{
	struct fpga_variant *variant;
	char *variant_name;

	variant_name = devm_kstrdup(&region->dev, name, GFP_KERNEL);
	if (!variant_name)
		return -ENOMEM;

	variant = devm_kzalloc(&region->dev, sizeof(*variant), GFP_KERNEL);
	if (!variant)
		return -ENOMEM;

	variant->priv = priv;
	variant->name = variant_name;

	mutex_lock(&region->variant_mutex);
	if (is_enabled)
		region->enabled_variant = variant;

	list_add_tail(&variant->node, &region->variant_list);
	mutex_unlock(&region->variant_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(fpga_region_add_variant);

/**
 * fpga_region_program_variant - switch the variant of an FPGA region
 * @region: FPGA region
 * @name: string identifier of the variant to enable
 *
 * Get the requested variant from the region's variant list. If found,
 * and it is not already active, removes the currently enabled variant via
 * the remove_variant() callback and applies the requested one  via the
 * apply_variant() callback.
 *
 * Return: 0 on success, -EINVAL if the variant is not found, or a negative
 * error code passed up from the apply_variant() callback.
 */
int fpga_region_program_variant(struct fpga_region *region, const char *name)
{
	struct fpga_variant *variant;
	bool found_variant = false;
	int ret = -EINVAL;

	mutex_lock(&region->variant_mutex);

	list_for_each_entry(variant, &region->variant_list, node) {
		if (sysfs_streq(variant->name, name)) {
			found_variant = true;
			break;
		}
	}

	if (!found_variant)
		goto out_unlock;

	if (region->enabled_variant == variant) {
		ret = 0;
		goto out_unlock;
	}

	if (region->enabled_variant && region->remove_variant)
		region->remove_variant(region, region->enabled_variant);

	if (region->apply_variant) {
		ret = region->apply_variant(region, variant);
		if (ret) {
			dev_err(&region->dev, "Variant programming failed!\n");
			region->enabled_variant = NULL;
		} else {
			region->enabled_variant = variant;
		}
	}

out_unlock:
	mutex_unlock(&region->variant_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(fpga_region_program_variant);

/**
 * fpga_region_program_fpga - program FPGA
 *
 * @region: FPGA region
 *
 * Program an FPGA using fpga image info (region->info).
 * If the region has a get_bridges function, the exclusive reference for the
 * bridges will be held if programming succeeds.  This is intended to prevent
 * reprogramming the region until the caller considers it safe to do so.
 * The caller will need to call fpga_bridges_put() before attempting to
 * reprogram the region.
 *
 * Return: 0 for success or negative error code.
 */
int fpga_region_program_fpga(struct fpga_region *region)
{
	struct device *dev = &region->dev;
	struct fpga_image_info *info = region->info;
	int ret;

	region = fpga_region_get(region);
	if (IS_ERR(region)) {
		dev_err(dev, "failed to get FPGA region\n");
		return PTR_ERR(region);
	}

	ret = fpga_mgr_lock(region->mgr);
	if (ret) {
		dev_err(dev, "FPGA manager is busy\n");
		goto err_put_region;
	}

	/*
	 * In some cases, we already have a list of bridges in the
	 * fpga region struct.  Or we don't have any bridges.
	 */
	if (region->get_bridges) {
		ret = region->get_bridges(region);
		if (ret) {
			dev_err(dev, "failed to get fpga region bridges\n");
			goto err_unlock_mgr;
		}
	}

	ret = fpga_bridges_disable(&region->bridge_list);
	if (ret) {
		dev_err(dev, "failed to disable bridges\n");
		goto err_put_br;
	}

	ret = fpga_mgr_load(region->mgr, info);
	if (ret) {
		dev_err(dev, "failed to load FPGA image\n");
		goto err_put_br;
	}

	ret = fpga_bridges_enable(&region->bridge_list);
	if (ret) {
		dev_err(dev, "failed to enable region bridges\n");
		goto err_put_br;
	}

	fpga_mgr_unlock(region->mgr);
	fpga_region_put(region);

	return 0;

err_put_br:
	if (region->get_bridges)
		fpga_bridges_put(&region->bridge_list);
err_unlock_mgr:
	fpga_mgr_unlock(region->mgr);
err_put_region:
	fpga_region_put(region);

	return ret;
}
EXPORT_SYMBOL_GPL(fpga_region_program_fpga);

static ssize_t compat_id_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct fpga_region *region = to_fpga_region(dev);

	if (!region->compat_id)
		return -ENOENT;

	return sprintf(buf, "%016llx%016llx\n",
		       (unsigned long long)region->compat_id->id_h,
		       (unsigned long long)region->compat_id->id_l);
}

static DEVICE_ATTR_RO(compat_id);

static ssize_t variants_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fpga_region *region = to_fpga_region(dev);
	struct fpga_variant *variant;
	int len = 0;

	mutex_lock(&region->variant_mutex);
	list_for_each_entry(variant, &region->variant_list, node)
		len += sysfs_emit_at(buf, len, "%s\n", variant->name);
	mutex_unlock(&region->variant_mutex);

	return len;
}
static DEVICE_ATTR_RO(variants);

static ssize_t enabled_variant_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fpga_region *region = to_fpga_region(dev);
	ssize_t len = 0;

	mutex_lock(&region->variant_mutex);
	if (region->enabled_variant)
		len = sysfs_emit(buf, "%s\n", region->enabled_variant->name);
	mutex_unlock(&region->variant_mutex);

	return len;
}

static ssize_t enabled_variant_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct fpga_region *region = to_fpga_region(dev);
	int ret;

	ret = fpga_region_program_variant(region, buf);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(enabled_variant);

static struct attribute *fpga_region_variant_attrs[] = {
	&dev_attr_enabled_variant.attr,
	&dev_attr_variants.attr,
	NULL,
};

static umode_t fpga_region_variant_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct fpga_region *region = to_fpga_region(dev);

	return region->apply_variant ? attr->mode : 0;
}

static const struct attribute_group fpga_region_variant_group = {
	.attrs = fpga_region_variant_attrs,
	.is_visible = fpga_region_variant_is_visible,
};

static struct attribute *fpga_region_attrs[] = {
	&dev_attr_compat_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpga_region);

static const struct attribute_group *fpga_region_variant_groups[] = {
	&fpga_region_variant_group,
	NULL,
};

/**
 * __fpga_region_register_full - create and register an FPGA Region device
 * @parent: device parent
 * @info: parameters for FPGA Region
 * @owner: module containing the get_bridges function
 *
 * Return: struct fpga_region or ERR_PTR()
 */
struct fpga_region *
__fpga_region_register_full(struct device *parent, const struct fpga_region_info *info,
			    struct module *owner)
{
	struct fpga_region *region;
	int id, ret = 0;

	if (!info) {
		dev_err(parent,
			"Attempt to register without required info structure\n");
		return ERR_PTR(-EINVAL);
	}

	region = kzalloc_obj(*region);
	if (!region)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc(&fpga_region_ida, GFP_KERNEL);
	if (id < 0) {
		ret = id;
		goto err_free;
	}

	region->mgr = info->mgr;
	region->compat_id = info->compat_id;
	region->priv = info->priv;
	region->get_bridges = info->get_bridges;
	region->ops_owner = owner;
	region->apply_variant = info->apply_variant;
	region->remove_variant = info->remove_variant;

	mutex_init(&region->mutex);
	mutex_init(&region->variant_mutex);
	INIT_LIST_HEAD(&region->bridge_list);
	INIT_LIST_HEAD(&region->variant_list);

	region->dev.class = &fpga_region_class;
	region->dev.parent = parent;
	region->dev.of_node = parent->of_node;
	region->dev.id = id;
	region->dev.groups = fpga_region_variant_groups;

	ret = dev_set_name(&region->dev, "region%d", id);
	if (ret)
		goto err_remove;

	ret = device_register(&region->dev);
	if (ret) {
		put_device(&region->dev);
		return ERR_PTR(ret);
	}

	return region;

err_remove:
	ida_free(&fpga_region_ida, id);
err_free:
	kfree(region);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(__fpga_region_register_full);

/**
 * __fpga_region_register - create and register an FPGA Region device
 * @parent: device parent
 * @mgr: manager that programs this region
 * @get_bridges: optional function to get bridges to a list
 * @owner: module containing the get_bridges function
 *
 * This simple version of the register function should be sufficient for most users.
 * The fpga_region_register_full() function is available for users that need to
 * pass additional, optional parameters.
 *
 * Return: struct fpga_region or ERR_PTR()
 */
struct fpga_region *
__fpga_region_register(struct device *parent, struct fpga_manager *mgr,
		       int (*get_bridges)(struct fpga_region *), struct module *owner)
{
	struct fpga_region_info info = { 0 };

	info.mgr = mgr;
	info.get_bridges = get_bridges;

	return __fpga_region_register_full(parent, &info, owner);
}
EXPORT_SYMBOL_GPL(__fpga_region_register);

/**
 * fpga_region_unregister - unregister an FPGA region
 * @region: FPGA region
 *
 * This function is intended for use in an FPGA region driver's remove function.
 */
void fpga_region_unregister(struct fpga_region *region)
{
	mutex_lock(&region->variant_mutex);
	if (region->enabled_variant && region->remove_variant) {
		region->remove_variant(region, region->enabled_variant);
		region->enabled_variant = NULL;
	}
	mutex_unlock(&region->variant_mutex);

	device_unregister(&region->dev);
}
EXPORT_SYMBOL_GPL(fpga_region_unregister);

static void fpga_region_dev_release(struct device *dev)
{
	struct fpga_region *region = to_fpga_region(dev);

	ida_free(&fpga_region_ida, region->dev.id);
	kfree(region);
}

static const struct class fpga_region_class = {
	.name = "fpga_region",
	.dev_groups = fpga_region_groups,
	.dev_release = fpga_region_dev_release,
};

/**
 * fpga_region_init - creates the fpga_region class.
 *
 * Return: 0 on success or ERR_PTR() on error.
 */
static int __init fpga_region_init(void)
{
	return class_register(&fpga_region_class);
}

static void __exit fpga_region_exit(void)
{
	class_unregister(&fpga_region_class);
	ida_destroy(&fpga_region_ida);
}

subsys_initcall(fpga_region_init);
module_exit(fpga_region_exit);

MODULE_DESCRIPTION("FPGA Region");
MODULE_AUTHOR("Alan Tull <atull@kernel.org>");
MODULE_LICENSE("GPL v2");
