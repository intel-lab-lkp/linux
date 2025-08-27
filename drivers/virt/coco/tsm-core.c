// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tsm.h>
#include <linux/idr.h>
#include <linux/rwsem.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/cleanup.h>
#include <linux/pci-tsm.h>

static struct class *tsm_class;
static DECLARE_RWSEM(tsm_rwsem);
static DEFINE_IDR(tsm_idr);

struct tsm_dev {
	struct device dev;
	int id;
	const struct pci_tsm_ops *pci_ops;
};

const char *tsm_name(const struct tsm_dev *tsm_dev)
{
	return dev_name(&tsm_dev->dev);
}

/* Caller responsible for ensuring it does not race tsm_dev unregistration */
struct tsm_dev *find_tsm_dev(int id)
{
	guard(rcu)();
	return idr_find(&tsm_idr, id);
}

const struct pci_tsm_ops *tsm_pci_ops(const struct tsm_dev *tsm_dev)
{
	if (!tsm_dev)
		return NULL;
	return tsm_dev->pci_ops;
}

static struct tsm_dev *alloc_tsm_dev(struct device *parent)
{
	struct tsm_dev *tsm_dev __free(kfree) =
		kzalloc(sizeof(*tsm_dev), GFP_KERNEL);
	struct device *dev;
	int id;

	if (!tsm_dev)
		return ERR_PTR(-ENOMEM);

	guard(rwsem_write)(&tsm_rwsem);
	id = idr_alloc(&tsm_idr, tsm_dev, 0, INT_MAX, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(id);

	tsm_dev->id = id;
	dev = &tsm_dev->dev;
	dev->parent = parent;
	dev->class = tsm_class;
	device_initialize(dev);
	return no_free_ptr(tsm_dev);
}

static struct tsm_dev *tsm_register_pci_or_reset(struct tsm_dev *tsm_dev,
						 struct pci_tsm_ops *pci_ops)
{
	int rc;

	if (!pci_ops)
		return tsm_dev;

	pci_ops->owner = tsm_dev;
	tsm_dev->pci_ops = pci_ops;
	rc = pci_tsm_register(tsm_dev);
	if (rc) {
		dev_err(tsm_dev->dev.parent,
			"PCI/TSM registration failure: %d\n", rc);
		device_unregister(&tsm_dev->dev);
		return ERR_PTR(rc);
	}

	/* Notify TSM userspace that PCI/TSM operations are now possible */
	kobject_uevent(&tsm_dev->dev.kobj, KOBJ_CHANGE);
	return tsm_dev;
}

static void put_tsm_dev(struct tsm_dev *tsm_dev)
{
	if (!IS_ERR_OR_NULL(tsm_dev))
		put_device(&tsm_dev->dev);
}

DEFINE_FREE(put_tsm_dev, struct tsm_dev *,
	    if (!IS_ERR_OR_NULL(_T)) put_tsm_dev(_T))

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

	return tsm_register_pci_or_reset(no_free_ptr(tsm_dev), pci_ops);
}
EXPORT_SYMBOL_GPL(tsm_register);

void tsm_unregister(struct tsm_dev *tsm_dev)
{
	pci_tsm_unregister(tsm_dev);
	device_unregister(&tsm_dev->dev);
}
EXPORT_SYMBOL_GPL(tsm_unregister);

static void tsm_release(struct device *dev)
{
	struct tsm_dev *tsm_dev = container_of(dev, typeof(*tsm_dev), dev);

	guard(rwsem_write)(&tsm_rwsem);
	idr_remove(&tsm_idr, tsm_dev->id);
	kfree(tsm_dev);
}

static int __init tsm_init(void)
{
	tsm_class = class_create("tsm");
	if (IS_ERR(tsm_class))
		return PTR_ERR(tsm_class);

	tsm_class->dev_release = tsm_release;
	return 0;
}
module_init(tsm_init)

static void __exit tsm_exit(void)
{
	class_destroy(tsm_class);
}
module_exit(tsm_exit)

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEE Security Manager Class Device");
