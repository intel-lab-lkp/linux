// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2025 Advanced Micro Devices, Inc. */
#include <linux/device.h>
#include <linux/pci.h>
#include "cxlpci.h"

#include "../cxlcache.h"
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

static const struct device_type cxl_cachedev_type = {
	.name = "cxl_cachedev",
	.release = cxl_cachedev_release,
	.devnode = cxl_cachedev_devnode,
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
