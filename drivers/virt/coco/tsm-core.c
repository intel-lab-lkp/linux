// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024-2025 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tsm.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/cleanup.h>
#include <linux/pci-tsm.h>

/* The caller must hold a tsm_get_device() reference for the entire use. */
const struct iommufd_viommu_ops *tsm_viommu_get_ops(struct tsm_dev *tsm_dev,
		struct device *dev, enum iommu_viommu_type type)
{
	if (!tsm_dev->pci_ops->viommu_get_ops)
		return NULL;
	return tsm_dev->pci_ops->viommu_get_ops(dev, type);
}
EXPORT_SYMBOL_GPL(tsm_viommu_get_ops);

static void tsm_release(struct device *);
static const struct class tsm_class = {
	.name		= "tsm",
	.dev_release	= tsm_release
};
static DEFINE_IDA(tsm_ida);

/**
 * tsm_get_device() - Pin the TSM attached to a device
 * @dev: device whose TSM is to be pinned
 *
 * A successful lookup pins the backend module. Backends providing vIOMMU
 * operations must not unregister their TSM independently of module unload.
 * Return: NULL if no vIOMMU-capable TSM is attached, an error if the module
 * cannot be pinned, or the TSM device associated with the pinned backend.
 */
struct tsm_dev *tsm_get_device(struct device *dev)
{
	if (!dev_is_pci(dev))
		return NULL;
	return pci_tsm_get_device(to_pci_dev(dev));
}
EXPORT_SYMBOL_GPL(tsm_get_device);

void tsm_put_device(struct tsm_dev *tsm_dev)
{
	module_put(tsm_dev->pci_ops->owner);
}
EXPORT_SYMBOL_GPL(tsm_put_device);

static int match_id(struct device *dev, const void *data)
{
	struct tsm_dev *tsm_dev = container_of(dev, struct tsm_dev, dev);
	int id = *(const int *)data;

	return tsm_dev->id == id;
}

struct tsm_dev *find_tsm_dev(int id)
{
	struct device *dev = class_find_device(&tsm_class, NULL, &id, match_id);

	if (!dev)
		return NULL;
	return container_of(dev, struct tsm_dev, dev);
}

static struct tsm_dev *alloc_tsm_dev(struct device *parent)
{
	struct device *dev;
	int id;

	struct tsm_dev *tsm_dev __free(kfree) =
		kzalloc_obj(*tsm_dev);
	if (!tsm_dev)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc(&tsm_ida, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);

	tsm_dev->id = id;
	dev = &tsm_dev->dev;
	dev->parent = parent;
	dev->class = &tsm_class;
	device_initialize(dev);

	return no_free_ptr(tsm_dev);
}

static int tsm_register_pci(struct tsm_dev *tsm_dev, struct pci_tsm_ops *pci_ops)
{
	int rc;

	if (!pci_ops)
		return 0;

	tsm_dev->pci_ops = pci_ops;
	rc = pci_tsm_register(tsm_dev);
	if (rc) {
		tsm_dev->pci_ops = NULL;
		dev_err(tsm_dev->dev.parent,
			"PCI/TSM registration failure: %d\n", rc);
		return rc;
	}

	/* Notify TSM userspace that PCI/TSM operations are now possible */
	kobject_uevent(&tsm_dev->dev.kobj, KOBJ_CHANGE);
	return 0;
}

struct tsm_dev *tsm_register(struct device *parent, struct pci_tsm_ops *pci_ops)
{
	struct tsm_dev *tsm_dev __free(put_tsm_dev) = alloc_tsm_dev(parent);
	struct device *dev;
	int rc;

	if (IS_ERR(tsm_dev))
		return tsm_dev;

	dev = &tsm_dev->dev;
	rc = dev_set_name(dev, "tsm%d", tsm_dev->id);
	if (rc)
		return ERR_PTR(rc);

	rc = device_add(dev);
	if (rc)
		return ERR_PTR(rc);

	rc = tsm_register_pci(tsm_dev, pci_ops);
	if (rc) {
		device_del(dev);
		return ERR_PTR(rc);
	}
	return no_free_ptr(tsm_dev);
}
EXPORT_SYMBOL_GPL(tsm_register);

void tsm_unregister(struct tsm_dev *tsm_dev)
{
	/* Remove the class lookup first. */
	device_del(&tsm_dev->dev);
	if (tsm_dev->pci_ops)
		pci_tsm_unregister(tsm_dev);
	put_device(&tsm_dev->dev);
}
EXPORT_SYMBOL_GPL(tsm_unregister);

static void tsm_release(struct device *dev)
{
	struct tsm_dev *tsm_dev = container_of(dev, typeof(*tsm_dev), dev);

	ida_free(&tsm_ida, tsm_dev->id);
	kfree(tsm_dev);
}

static int __init tsm_init(void)
{
	return class_register(&tsm_class);
}
module_init(tsm_init)

static void __exit tsm_exit(void)
{
	class_unregister(&tsm_class);
}
module_exit(tsm_exit)

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEE Security Manager Class Device");
