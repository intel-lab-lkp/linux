// SPDX-License-Identifier: GPL-2.0
/*
 * FPGA Region - Support for FPGA programming under Linux
 *
 *  Copyright (C) 2013-2016 Altera Corporation
 *  Copyright (C) 2017 Intel Corporation
 */
#include <linux/configfs.h>
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

static struct attribute *fpga_region_attrs[] = {
	&dev_attr_compat_id.attr,
	NULL,
};
ATTRIBUTE_GROUPS(fpga_region);

static struct fpga_region *item_to_fpga_region(struct config_item *item)
{
	return container_of(to_configfs_subsystem(to_config_group(item)),
			    struct fpga_region, subsys);
}

/**
 * fpga_region_image_store - Set firmware image name for FPGA region
 * This function sets the firmware image name for an FPGA region through configfs.
 * @item: Configfs item representing the FPGA region
 * @buf: Input buffer containing the firmware image name
 * @count: Size of the input buffer
 *
 * Return: Number of bytes written on success, or negative errno on failure.
 */
static ssize_t fpga_region_image_store(struct config_item *item, const char *buf, size_t count)
{
	struct fpga_region *region = item_to_fpga_region(item);
	struct device *dev = &region->dev;
	struct fpga_image_info *info;
	char firmware_name[NAME_MAX];
	char *s;

	if (region->info) {
		dev_err(dev, "Region already has already configured.\n");
		return -EINVAL;
	}

	info = fpga_image_info_alloc(dev);
	if (!info)
		return -ENOMEM;

	/* copy to path buffer (and make sure it's always zero terminated */
	count = snprintf(firmware_name, sizeof(firmware_name) - 1, "%s", buf);
	firmware_name[sizeof(firmware_name) - 1] = '\0';

	/* strip trailing newlines */
	s = firmware_name + strlen(firmware_name);
	while (s > firmware_name && *--s == '\n')
		*s = '\0';

	region->firmware_name = devm_kstrdup(dev, firmware_name, GFP_KERNEL);
	if (!region->firmware_name)
		return -ENOMEM;

	region->info = info;

	return count;
}

/**
 * fpga_region_config_store - Trigger FPGA configuration via configfs
 * @item: Configfs item representing the FPGA region
 * @buf: Input buffer containing the configuration command (expects "1" to program, "0" to remove)
 * @count: Size of the input buffer
 *
 * If the input is "1", this function performs:
 *   1. region_pre_config() (if defined)
 *   2. Bitstream programming via fpga_region_program_fpga() (unless external config flag is set)
 *   3. region_post_config() (if defined)
 *
 * If the input is "0", it triggers region_remove() (if defined).
 *
 * Return: Number of bytes processed on success, or negative errno on failure.
 */
static ssize_t fpga_region_config_store(struct config_item *item,
					const char *buf, size_t count)
{
	struct fpga_region *region = item_to_fpga_region(item);
	int config_value, ret = 0;

	/* Parse input: must be "0" or "1" */
	if (kstrtoint(buf, 10, &config_value) || (config_value != 0 && config_value != 1))
		return -EINVAL;

	/* Ensure fpga_image_info is available */
	if (!region->info)
		return -EINVAL;

	if (config_value == 1) {
		/* Pre-config */
		if (region->region_ops->region_pre_config) {
			ret = region->region_ops->region_pre_config(region);
			if (ret)
				return ret;
		}

		/* Program bitstream if not external */
		if (!(region->info->flags & FPGA_MGR_EXTERNAL_CONFIG)) {
			ret = fpga_region_program_fpga(region);
			if (ret)
				return ret;
		}

		/* Post-config */
		if (region->region_ops->region_post_config) {
			ret = region->region_ops->region_post_config(region);
			if (ret)
				return ret;
		}

	} else {
		/* Remove configuration */
		if (region->region_ops->region_remove) {
			ret = region->region_ops->region_remove(region);
			if (ret)
				return ret;
		}
	}

	return count;
}

/* Define Attributes */
CONFIGFS_ATTR_WO(fpga_region_, image);
CONFIGFS_ATTR_WO(fpga_region_, config);

/* Attribute List */
static struct configfs_attribute *fpga_region_config_attrs[] = {
	&fpga_region_attr_image,
	&fpga_region_attr_config,
	NULL,
};

/* ConfigFS Item Type */
static const struct config_item_type fpga_region_item_type = {
	.ct_attrs = fpga_region_config_attrs,
	.ct_owner = THIS_MODULE,
};

static int fpga_region_configfs_register(struct fpga_region *region)
{
	struct configfs_subsystem *subsys = &region->subsys;

	snprintf(subsys->su_group.cg_item.ci_namebuf,
		 sizeof(subsys->su_group.cg_item.ci_namebuf),
		 "%s", dev_name(&region->dev));

	subsys->su_group.cg_item.ci_type = &fpga_region_item_type;

	config_group_init(&subsys->su_group);

	return configfs_register_subsystem(subsys);
}

static void fpga_region_configfs_unregister(struct fpga_region *region)
{
	struct configfs_subsystem *subsys = &region->subsys;

	configfs_unregister_subsystem(subsys);
}

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

	region = kzalloc(sizeof(*region), GFP_KERNEL);
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

	mutex_init(&region->mutex);
	INIT_LIST_HEAD(&region->bridge_list);

	region->dev.class = &fpga_region_class;
	region->dev.parent = parent;
	region->dev.of_node = parent->of_node;
	region->dev.id = id;

	ret = dev_set_name(&region->dev, "region%d", id);
	if (ret)
		goto err_remove;

	if (info->region_ops) {
		region->region_ops = info->region_ops;
		ret = fpga_region_configfs_register(region);
		if (ret)
			goto err_remove;
	}

	ret = device_register(&region->dev);
	if (ret) {
		fpga_region_configfs_unregister(region);
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
 * __fpga_region_register_with_ops - create and register an FPGA Region device
 * with user interface call-backs.
 * @parent: device parent
 * @mgr: manager that programs this region
 * @region_ops: ops for low level FPGA region for device enumeration/removal
 * @priv: of-fpga-region private data
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
__fpga_region_register_with_ops(struct device *parent, struct fpga_manager *mgr,
				const struct fpga_region_ops *region_ops,
				void *priv,
				int (*get_bridges)(struct fpga_region *),
				struct module *owner)
{
	struct fpga_region_info info = { 0 };

	info.mgr = mgr;
	info.priv = priv;
	info.get_bridges = get_bridges;
	info.region_ops = region_ops;

	return __fpga_region_register_full(parent, &info, owner);
}
EXPORT_SYMBOL_GPL(__fpga_region_register_with_ops);

/**
 * fpga_region_unregister - unregister an FPGA region
 * @region: FPGA region
 *
 * This function is intended for use in an FPGA region driver's remove function.
 */
void fpga_region_unregister(struct fpga_region *region)
{
	fpga_region_configfs_unregister(region);
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
