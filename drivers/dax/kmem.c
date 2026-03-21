// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2016-2019 Intel Corporation. All rights reserved. */
#include <linux/memremap.h>
#include <linux/pagemap.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/memory-tiers.h>
#include <linux/memory_hotplug.h>
#include <linux/string_helpers.h>
#include "dax-private.h"
#include "bus.h"

/* Internal function exported only to kmem module */
extern int __add_memory_driver_managed(int nid, u64 start, u64 size,
				       const char *resource_name,
				       mhp_t mhp_flags, enum mmop online_type);

/* Memory resource name used for add_memory_driver_managed(). */
static const char *kmem_name;
/* Set if any memory will remain added when the driver will be unloaded. */
static bool any_hotremove_failed;

static int dax_kmem_range(struct dev_dax *dev_dax, int i, struct range *r)
{
	struct dev_dax_range *dax_range = &dev_dax->ranges[i];
	struct range *range = &dax_range->range;

	*r = memory_block_align_range(range);
	if (r->start >= r->end) {
		r->start = range->start;
		r->end = range->end;
		return -ENOSPC;
	}
	return 0;
}

#define DAX_KMEM_UNPLUGGED	(-1)

struct dax_kmem_data {
	const char *res_name;
	int mgid;
	struct memory_dev_type *mtype;
	int numa_node;
	struct dev_dax *dev_dax;
	int state;
	struct mutex lock; /* protects hotplug state transitions */
	struct resource *res[];
};

/**
 * dax_kmem_do_hotplug - hotplug memory for dax kmem device
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 * @online_type: MMOP_ONLINE or MMOP_ONLINE_MOVABLE
 *
 * Hotplugs all ranges in the dev_dax region as system memory using
 * the specified online type.
 *
 * Returns the number of successfully mapped ranges, or negative error.
 */
static int dax_kmem_do_hotplug(struct dev_dax *dev_dax,
			       struct dax_kmem_data *data,
			       int online_type)
{
	struct device *dev = &dev_dax->dev;
	int i, rc, onlined = 0;
	mhp_t mhp_flags;

	if (data->state == MMOP_ONLINE || data->state == MMOP_ONLINE_MOVABLE)
		return -EINVAL;

	if (online_type != MMOP_ONLINE && online_type != MMOP_ONLINE_MOVABLE)
		return -EINVAL;

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;

		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc)
			continue;

		mhp_flags = MHP_NID_IS_MGID;
		if (dev_dax->memmap_on_memory)
			mhp_flags |= MHP_MEMMAP_ON_MEMORY;

		/*
		 * Ensure that future kexec'd kernels will not treat
		 * this as RAM automatically.
		 */
		rc = __add_memory_driver_managed(data->mgid, range.start,
				range_len(&range), kmem_name, mhp_flags,
				online_type);

		if (rc) {
			dev_warn(dev, "mapping%d: %#llx-%#llx memory add failed\n",
				 i, range.start, range.end);
			if (onlined)
				continue;
			return rc;
		}
		onlined++;
	}

	return onlined;
}

/**
 * dax_kmem_init_resources - create memory regions for dax kmem
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 *
 * Initializes all the resources for the DAX
 *
 * Returns the number of successfully mapped ranges, or negative error.
 */
static int dax_kmem_init_resources(struct dev_dax *dev_dax,
				   struct dax_kmem_data *data)
{
	struct device *dev = &dev_dax->dev;
	int i, rc, mapped = 0;

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct resource *res;
		struct range range;

		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc)
			continue;

		/* Skip ranges already added */
		if (data->res[i])
			continue;

		/* Region is permanently reserved if hotremove fails. */
		res = request_mem_region(range.start, range_len(&range),
					 data->res_name);
		if (!res) {
			dev_warn(dev, "mapping%d: %#llx-%#llx could not reserve region\n",
				 i, range.start, range.end);
			/*
			 * Once some memory has been onlined we can't
			 * assume that it can be un-onlined safely.
			 */
			if (mapped)
				continue;
			return -EBUSY;
		}
		data->res[i] = res;
		/*
		 * Set flags appropriate for System RAM.  Leave ..._BUSY clear
		 * so that add_memory() can add a child resource.  Do not
		 * inherit flags from the parent since it may set new flags
		 * unknown to us that will break add_memory() below.
		 */
		res->flags = IORESOURCE_SYSTEM_RAM;
		mapped++;
	}
	return mapped;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/**
 * dax_kmem_do_hotremove - hot-remove memory for dax kmem device
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 *
 * Offlines and removes all ranges in the dev_dax region.
 *
 * Returns the number of successfully removed ranges, or negative error.
 */
static int dax_kmem_do_hotremove(struct dev_dax *dev_dax,
				 struct dax_kmem_data *data)
{
	struct device *dev = &dev_dax->dev;
	int i, success = 0;

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;
		int rc;

		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc)
			continue;

		/* Skip ranges not currently added */
		if (!data->res[i])
			continue;

		rc = offline_and_remove_memory(range.start, range_len(&range));
		if (rc == 0) {
			/* Release the resource for the successfully removed range */
			remove_resource(data->res[i]);
			kfree(data->res[i]);
			data->res[i] = NULL;
			success++;
			continue;
		}
		any_hotremove_failed = true;
		dev_err(dev, "mapping%d: %#llx-%#llx hotremove failed\n",
			i, range.start, range.end);
	}

	return success;
}
#else
static int dax_kmem_do_hotremove(struct dev_dax *dev_dax,
				 struct dax_kmem_data *data)
{
	return -EBUSY;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

/**
 * dax_kmem_cleanup_resources - remove the dax memory resources
 * @dev_dax: the dev_dax instance
 * @data: the dax_kmem_data structure with resource tracking
 *
 * Removes all resources in the dev_dax region.
 */
static void dax_kmem_cleanup_resources(struct dev_dax *dev_dax,
				       struct dax_kmem_data *data)
{
	int i;

	/*
	 * If the device unbind occurs before memory is hotremoved, we can never
	 * remove the memory (requires reboot).  Attempting an offline operation
	 * here may cause deadlock and a failure to finish the unbind.
	 *
	 * This WARN used to be a BUG called by remove_memory().
	 *
	 * Note: This leaks the resources.
	 */
	if (WARN(((data->state != DAX_KMEM_UNPLUGGED) &&
		  (data->state != MMOP_OFFLINE)),
		 "Hotplug memory regions stuck online until reboot"))
		return;

	for (i = 0; i < dev_dax->nr_range; i++) {
		if (!data->res[i])
			continue;
		remove_resource(data->res[i]);
		kfree(data->res[i]);
		data->res[i] = NULL;
	}
}

static int dax_kmem_parse_state(const char *buf)
{
	if (sysfs_streq(buf, "unplug"))
		return DAX_KMEM_UNPLUGGED;
	if (sysfs_streq(buf, "online"))
		return MMOP_ONLINE;
	if (sysfs_streq(buf, "online_movable"))
		return MMOP_ONLINE_MOVABLE;
	return -EINVAL;
}

static ssize_t hotplug_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	const char *state_str;

	if (!data)
		return -ENXIO;

	switch (data->state) {
	case DAX_KMEM_UNPLUGGED:
		state_str = "unplugged";
		break;
	case MMOP_OFFLINE:
		state_str = "offline";
		break;
	case MMOP_ONLINE:
		state_str = "online";
		break;
	case MMOP_ONLINE_MOVABLE:
		state_str = "online_movable";
		break;
	default:
		state_str = "unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", state_str);
}

static ssize_t hotplug_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct dev_dax *dev_dax = to_dev_dax(dev);
	struct dax_kmem_data *data = dev_get_drvdata(dev);
	int online_type;
	int rc;

	if (!data)
		return -ENXIO;

	online_type = dax_kmem_parse_state(buf);
	if (online_type < DAX_KMEM_UNPLUGGED)
		return online_type;

	guard(mutex)(&data->lock);

	/* Already in requested state */
	if (data->state == online_type)
		return len;

	if (online_type == DAX_KMEM_UNPLUGGED) {
		rc = dax_kmem_do_hotremove(dev_dax, data);
		if (rc < 0) {
			dev_warn(dev, "hotplug state is inconsistent\n");
			return rc;
		}
		if (rc < dev_dax->nr_range)
			dev_warn(dev, "partial hotremove: %d of %d ranges removed\n",
				 rc, dev_dax->nr_range);
		else
			data->state = DAX_KMEM_UNPLUGGED;
		return len;
	}

	/*
	 * online_type is MMOP_ONLINE or MMOP_ONLINE_MOVABLE
	 * Cannot switch between online types without unplugging first
	 */
	if (data->state == MMOP_ONLINE || data->state == MMOP_ONLINE_MOVABLE)
		return -EBUSY;

	rc = dax_kmem_do_hotplug(dev_dax, data, online_type);
	if (rc < 0)
		return rc;

	data->state = online_type;
	return len;
}
static DEVICE_ATTR_RW(hotplug);

static int dev_dax_kmem_probe(struct dev_dax *dev_dax)
{
	struct device *dev = &dev_dax->dev;
	unsigned long total_len = 0, orig_len = 0;
	struct dax_kmem_data *data;
	struct memory_dev_type *mtype;
	int i, rc;
	int numa_node;
	int adist = MEMTIER_DEFAULT_LOWTIER_ADISTANCE;

	/*
	 * Ensure good NUMA information for the persistent memory.
	 * Without this check, there is a risk that slow memory
	 * could be mixed in a node with faster memory, causing
	 * unavoidable performance issues.
	 */
	numa_node = dev_dax->target_node;
	if (numa_node < 0) {
		dev_warn(dev, "rejecting DAX region with invalid node: %d\n",
				numa_node);
		return -EINVAL;
	}

	mt_calc_adistance(numa_node, &adist);
	mtype = mt_get_memory_type(adist);
	if (IS_ERR(mtype))
		return PTR_ERR(mtype);

	for (i = 0; i < dev_dax->nr_range; i++) {
		struct range range;

		orig_len += range_len(&dev_dax->ranges[i].range);
		rc = dax_kmem_range(dev_dax, i, &range);
		if (rc) {
			dev_info(dev, "mapping%d: %#llx-%#llx too small after alignment\n",
					i, range.start, range.end);
			continue;
		}
		total_len += range_len(&range);
	}

	if (!total_len) {
		dev_warn(dev, "rejecting DAX region without any memory after alignment\n");
		return -EINVAL;
	} else if (total_len != orig_len) {
		char buf[16];

		string_get_size(orig_len - total_len, 1, STRING_UNITS_2,
				buf, sizeof(buf));
		dev_warn(dev, "DAX region truncated by %s due to alignment\n", buf);
	}

	init_node_memory_type(numa_node, mtype);

	rc = -ENOMEM;
	data = kzalloc_flex(*data, res, dev_dax->nr_range);
	if (!data)
		goto err_dax_kmem_data;

	data->res_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!data->res_name)
		goto err_res_name;

	rc = memory_group_register_static(numa_node, PFN_UP(total_len));
	if (rc < 0)
		goto err_reg_mgid;
	data->mgid = rc;
	data->mtype = mtype;
	data->numa_node = numa_node;
	data->dev_dax = dev_dax;
	data->state = DAX_KMEM_UNPLUGGED;
	mutex_init(&data->lock);

	dev_set_drvdata(dev, data);

	rc = dax_kmem_init_resources(dev_dax, data);
	if (rc < 0)
		goto err_resources;

	/*
	 * Hotplug using the configured online type for this device.
	 */
	if (dev_dax->online_type != MMOP_OFFLINE ||
	    dev_dax->online_type == mhp_get_default_online_type()) {
		rc = dax_kmem_do_hotplug(dev_dax, data, dev_dax->online_type);
		if (rc < 0)
			goto err_hotplug;
		data->state = dev_dax->online_type;
	}

	rc = device_create_file(dev, &dev_attr_hotplug);
	if (rc)
		dev_warn(dev, "failed to create hotplug sysfs entry\n");

	return 0;

err_hotplug:
	dax_kmem_cleanup_resources(dev_dax, data);
err_resources:
	dev_set_drvdata(dev, NULL);
	memory_group_unregister(data->mgid);
err_reg_mgid:
	kfree(data->res_name);
err_res_name:
	kfree(data);
err_dax_kmem_data:
	clear_node_memory_type(numa_node, mtype);
	return rc;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
static void dev_dax_kmem_remove(struct dev_dax *dev_dax)
{
	int node = dev_dax->target_node;
	struct device *dev = &dev_dax->dev;
	struct dax_kmem_data *data = dev_get_drvdata(dev);

	device_remove_file(dev, &dev_attr_hotplug);
	dax_kmem_cleanup_resources(dev_dax, data);
	memory_group_unregister(data->mgid);
	kfree(data->res_name);
	kfree(data);
	dev_set_drvdata(dev, NULL);
	/*
	 * Clear the memtype association on successful unplug.
	 * If not, we have memory blocks left which can be
	 * offlined/onlined later. We need to keep memory_dev_type
	 * for that. This implies this reference will be around
	 * till next reboot.
	 */
	clear_node_memory_type(node, data->mtype);
}
#else
static void dev_dax_kmem_remove(struct dev_dax *dev_dax)
{
	struct device *dev = &dev_dax->dev;

	device_remove_file(dev, &dev_attr_hotplug);

	/*
	 * Without hotremove purposely leak the request_mem_region() for the
	 * device-dax range and return '0' to ->remove() attempts. The removal
	 * of the device from the driver always succeeds, but the region is
	 * permanently pinned as reserved by the unreleased
	 * request_mem_region().
	 */
	any_hotremove_failed = true;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

static struct dax_device_driver device_dax_kmem_driver = {
	.probe = dev_dax_kmem_probe,
	.remove = dev_dax_kmem_remove,
	.type = DAXDRV_KMEM_TYPE,
};

static int __init dax_kmem_init(void)
{
	int rc;

	/* Resource name is permanently allocated if any hotremove fails. */
	kmem_name = kstrdup_const("System RAM (kmem)", GFP_KERNEL);
	if (!kmem_name)
		return -ENOMEM;

	rc = dax_driver_register(&device_dax_kmem_driver);
	if (rc)
		goto error_dax_driver;

	return rc;

error_dax_driver:
	kfree_const(kmem_name);
	return rc;
}

static void __exit dax_kmem_exit(void)
{
	dax_driver_unregister(&device_dax_kmem_driver);
	if (!any_hotremove_failed)
		kfree_const(kmem_name);
}

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("KMEM DAX: map dax-devices as System-RAM");
MODULE_LICENSE("GPL v2");
module_init(dax_kmem_init);
module_exit(dax_kmem_exit);
MODULE_ALIAS_DAX_DEVICE(0);
