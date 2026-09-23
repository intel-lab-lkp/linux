// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Advanced Micro Devices, Inc. */
#include <linux/device.h>
#include <linux/pci.h>
#include <cxlcache.h>

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
	struct cxl_dev_state *cxlds = cxlcd->cxlds;

	cxlcd->cxlds = NULL;
	cxlds->cxlcd = NULL;
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

static void detach_cachedev(struct work_struct *work)
{
	struct cxl_cachedev *cxlcd;

	cxlcd = container_of(work, typeof(*cxlcd), detach_work);

	device_release_driver(&cxlcd->dev);
	put_device(&cxlcd->dev);
}

static struct lock_class_key cxl_cachedev_key;

static struct cxl_cachedev *cxl_cachedev_alloc(struct cxl_dev_state *cxlds)
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
	cxlcd->endpoint = ERR_PTR(-ENODEV);
	cxlcd->cache_id = CXL_CACHE_ID_NO_ID;

	dev = &cxlcd->dev;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_cachedev_key);
	dev->parent = cxlds->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_cachedev_type;
	device_set_pm_not_required(dev);
	INIT_WORK(&cxlcd->detach_work, detach_cachedev);

	return_ptr(cxlcd);
}

DEFINE_FREE(put_cxlcd, struct cxl_cachedev *,
	    if (!IS_ERR_OR_NULL(_T)) put_device(&_T->dev));

static struct cxl_cachedev *cxl_cachedev_autoremove(struct cxl_cachedev *cxlcd)
{
	int rc;

	rc = devm_add_action_or_reset(cxlcd->cxlds->dev,
				      cxl_cachedev_unregister, cxlcd);
	if (rc)
		return ERR_PTR(rc);

	return cxlcd;
}

struct cxl_cachedev *__devm_cxl_add_cachedev(struct cxl_dev_state *cxlds,
					     void *attach)
{
	struct device *dev;
	int rc;

	struct cxl_cachedev *cxlcd __free(put_cxlcd) =
		cxl_cachedev_alloc(cxlds);
	if (IS_ERR(cxlcd))
		return cxlcd;

	dev = &cxlcd->dev;
	rc = dev_set_name(dev, "cache%d", cxlcd->id);
	if (rc)
		return ERR_PTR(rc);

	cxlcd->cxlds = cxlds;
	cxlds->cxlcd = cxlcd;

	rc = device_add(dev);
	if (rc) {
		cxlds->cxlcd = NULL;
		return ERR_PTR(rc);
	}

	return cxl_cachedev_autoremove(no_free_ptr(cxlcd));
}
EXPORT_SYMBOL_FOR_MODULES(__devm_cxl_add_cachedev, "cxl_cache");
