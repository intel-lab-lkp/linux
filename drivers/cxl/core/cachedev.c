// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Advanced Micro Devices, Inc. */
#include <linux/string_helpers.h>
#include <linux/device.h>
#include <linux/pci.h>
#include "cxlpci.h"

#include "../cxlcache.h"
#include "../cxlpci.h"
#include "private.h"

static DEFINE_IDA(cxl_cachedev_ida);

static void cxl_cachedev_release(struct device *dev)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);

	ida_free(&cxl_cachedev_ida, cxlcd->id);
	kfree(cxlcd);
}

static void cxl_cachedev_unregister(void *dev)
{
	struct cxl_cachedev *cxlcd = dev;

	cxlcd->cxlds = NULL;
	device_del(&cxlcd->dev);
	put_device(&cxlcd->dev);
}

static char *cxl_cachedev_devnode(const struct device *dev, umode_t *mode,
				  kuid_t *uid, kgid_t *gid)
{
	return kasprintf(GFP_KERNEL, "cxl/%s", dev_name(dev));
}

static ssize_t numa_node_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	return sysfs_emit(buf, "%d\n", dev_to_node(dev));
}
static DEVICE_ATTR_RO(numa_node);

static struct attribute *cxl_cachedev_attributes[] = {
	&dev_attr_numa_node.attr,
	NULL
};

static umode_t cxl_cachedev_visible(struct kobject *kobj, struct attribute *a,
				    int n)
{
	if (!IS_ENABLED(CONFIG_NUMA) && a == &dev_attr_numa_node.attr)
		return 0;
	return a->mode;
}

static struct attribute_group cxl_cachedev_attribute_group = {
	.attrs = cxl_cachedev_attributes,
	.is_visible = cxl_cachedev_visible,
};

static ssize_t cache_size_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	struct cxl_cache_state cstate = cxlds->cstate;

	return sysfs_emit(buf, "%llu\n", cstate.size);
}
static DEVICE_ATTR_RO(cache_size);

static ssize_t cache_unit_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	struct cxl_cache_state cstate = cxlds->cstate;
	char unit_buf[32];
	int rc;

	rc = string_get_size(cstate.size, 1, STRING_UNITS_2, unit_buf,
			     sizeof(unit_buf) - 1);
	if (rc <= 0)
		return -ENXIO;

	return sysfs_emit(buf, "%s\n", unit_buf);
}
static DEVICE_ATTR_RO(cache_unit);

static struct attribute *cxl_cachedev_cache_attributes[] = {
	&dev_attr_cache_size.attr,
	&dev_attr_cache_unit.attr,
	NULL
};

static umode_t cxl_cachedev_cache_visible(struct kobject *kobj,
					  struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;

	if (!cxlds || cxlds->cstate.size == 0)
		return 0;
	return a->mode;
}

static struct attribute_group cxl_cachedev_cache_attribute_group = {
	.attrs = cxl_cachedev_cache_attributes,
	.is_visible = cxl_cachedev_cache_visible,
};

static ssize_t cache_disable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t n)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	bool disable;
	int rc;

	rc = kstrtobool(buf, &disable);
	if (rc)
		return rc;

	rc = cxl_accel_set_cache_disable(cxlds, disable);
	if (rc < 0)
		return rc;

	return n;
}

static ssize_t cache_disable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;

	return sysfs_emit(buf, "%d\n", cxl_accel_caching_disabled(cxlds));
}
static DEVICE_ATTR_RW(cache_disable);

static ssize_t cache_invalid_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;

	return sysfs_emit(buf, "%d\n", cxl_accel_cache_invalid(cxlds));
}
static DEVICE_ATTR_RO(cache_invalid);

static ssize_t init_wbinvd_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t n)
{
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	int rc;

	rc = cxl_accel_initiate_wbinvd(cxlds);
	return rc ? rc : n;
}
static DEVICE_ATTR_WO(init_wbinvd);

static struct attribute *cxl_cachedev_mgmt_attributes[] = {
	&dev_attr_cache_disable.attr,
	&dev_attr_cache_invalid.attr,
	&dev_attr_init_wbinvd.attr,
	NULL
};

static umode_t cxl_cachedev_mgmt_visible(struct kobject *kobj,
					 struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct cxl_cachedev *cxlcd = to_cxl_cachedev(dev);
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	struct pci_dev *pdev = to_pci_dev(cxlds->dev);
	u16 cap;
	int rc;

	rc = pci_read_config_word(pdev, cxlds->cxl_dvsec + CXL_DVSEC_CAP_OFFSET,
				  &cap);
	if (rc)
		return 0;

	if (!(cap & CXL_DVSEC_WBINVD_CAPABLE) &&
	    a == &dev_attr_init_wbinvd.attr)
		return 0;

	return a->mode;
}

static struct attribute_group cxl_cachedev_mgmt_attribute_group = {
	.attrs = cxl_cachedev_mgmt_attributes,
	.is_visible = cxl_cachedev_mgmt_visible,
};

static const struct attribute_group *cxl_cachedev_attribute_groups[] = {
	&cxl_cachedev_attribute_group,
	&cxl_cachedev_cache_attribute_group,
	&cxl_cachedev_mgmt_attribute_group,
	NULL
};

static const struct device_type cxl_cachedev_type = {
	.name = "cxl_cachedev",
	.release = cxl_cachedev_release,
	.devnode = cxl_cachedev_devnode,
	.groups = cxl_cachedev_attribute_groups,
};

bool is_cxl_cachedev(const struct device *dev)
{
	return dev->type == &cxl_cachedev_type;
}
EXPORT_SYMBOL_NS_GPL(is_cxl_cachedev, "CXL");

static struct lock_class_key cxl_cachedev_key;

struct cxl_cachedev *cxl_cachedev_alloc(struct cxl_dev_state *cxlds)
{
	struct device *dev;
	int rc;

	struct cxl_cachedev *cxlcd __free(kfree) =
		kzalloc(sizeof(*cxlcd), GFP_KERNEL);
	if (!cxlcd)
		return ERR_PTR(-ENOMEM);

	rc = ida_alloc(&cxl_cachedev_ida, GFP_KERNEL);
	if (rc < 0)
		return ERR_PTR(rc);

	cxlcd->id = rc;
	cxlcd->depth = -1;
	cxlcd->cxlds = cxlds;
	cxlds->cxlcd = cxlcd;
	cxlcd->endpoint = ERR_PTR(-ENXIO);

	dev = &cxlcd->dev;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_cachedev_key);
	dev->parent = cxlds->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_cachedev_type;
	device_set_pm_not_required(dev);

	return_ptr(cxlcd);
}
EXPORT_SYMBOL_NS_GPL(cxl_cachedev_alloc, "CXL");

struct cxl_cachedev *devm_cxl_cachedev_add_or_reset(struct device *host,
						    struct cxl_cachedev *cxlcd)
{
	int rc;

	rc = device_add(&cxlcd->dev);
	if (rc)
		return ERR_PTR(rc);

	rc = devm_add_action_or_reset(host, cxl_cachedev_unregister, cxlcd);
	if (rc)
		return ERR_PTR(rc);

	return cxlcd;
}
EXPORT_SYMBOL_NS_GPL(devm_cxl_cachedev_add_or_reset, "CXL");

bool cxl_cachedev_is_type2(struct cxl_cachedev *cxlcd)
{
	struct cxl_dev_state *cxlds = cxlcd->cxlds;
	int dvsec = cxlds->cxl_dvsec;
	u32 cap;
	int rc;

	if (!dev_is_pci(cxlds->dev))
		return false;

	rc = pci_read_config_dword(to_pci_dev(cxlds->dev),
				   dvsec + CXL_DVSEC_CAP_OFFSET, &cap);
	if (rc)
		return rc;

	return (cap & CXL_DVSEC_MEM_CAPABLE) && (cap & CXL_DVSEC_CACHE_CAPABLE);
}
EXPORT_SYMBOL_NS_GPL(cxl_cachedev_is_type2, "CXL");
