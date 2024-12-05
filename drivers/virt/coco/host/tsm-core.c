// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define dev_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tsm.h>
#include <linux/pci.h>
#include <linux/rwsem.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/cleanup.h>
#include <linux/pci-tsm.h>
#include <linux/pci-ide.h>

static DECLARE_RWSEM(tsm_core_rwsem);
static struct class *tsm_class;
static struct tsm_subsys {
	struct device dev;
	const struct pci_tsm_ops *pci_ops;
} *tsm_subsys;

static struct tsm_subsys *
alloc_tsm_subsys(struct device *parent, const struct attribute_group **groups)
{
	struct tsm_subsys *subsys = kzalloc(sizeof(*subsys), GFP_KERNEL);
	struct device *dev;

	if (!subsys)
		return ERR_PTR(-ENOMEM);
	dev = &subsys->dev;
	dev->parent = parent;
	dev->groups = groups;
	dev->class = tsm_class;
	device_initialize(dev);
	return subsys;
}

static void put_tsm_subsys(struct tsm_subsys *subsys)
{
	if (!IS_ERR_OR_NULL(subsys))
		put_device(&subsys->dev);
}

DEFINE_FREE(put_tsm_subsys, struct tsm_subsys *,
	    if (!IS_ERR_OR_NULL(_T)) put_tsm_subsys(_T))
struct tsm_subsys *tsm_register(struct device *parent,
				const struct attribute_group **groups,
				const struct pci_tsm_ops *pci_ops)
{
	struct device *dev;
	int rc;

	guard(rwsem_write)(&tsm_core_rwsem);
	if (tsm_subsys) {
		dev_warn(parent, "failed to register: %s already registered\n",
			 dev_name(tsm_subsys->dev.parent));
		return ERR_PTR(-EBUSY);
	}

	struct tsm_subsys *subsys __free(put_tsm_subsys) =
		alloc_tsm_subsys(parent, groups);
	if (IS_ERR(subsys))
		return subsys;

	dev = &subsys->dev;
	rc = dev_set_name(dev, "tsm0");
	if (rc)
		return ERR_PTR(rc);

	rc = pci_tsm_register(pci_ops, NULL);
	if (rc) {
		dev_err(parent, "PCI initialization failure: %pe\n",
			ERR_PTR(rc));
		return ERR_PTR(rc);
	}

	rc = device_add(dev);
	if (rc) {
		pci_tsm_unregister(pci_ops);
		return ERR_PTR(rc);
	}

	subsys->pci_ops = pci_ops;
	tsm_subsys = no_free_ptr(subsys);

	return tsm_subsys;
}
EXPORT_SYMBOL_GPL(tsm_register);

void tsm_unregister(struct tsm_subsys *subsys)
{
	guard(rwsem_write)(&tsm_core_rwsem);
	if (!tsm_subsys || subsys != tsm_subsys) {
		pr_warn("failed to unregister, not currently registered\n");
		return;
	}

	pci_tsm_unregister(subsys->pci_ops);
	device_unregister(&subsys->dev);

	tsm_subsys = NULL;
}
EXPORT_SYMBOL_GPL(tsm_unregister);

/* must be invoked between tsm_register / tsm_unregister */
int tsm_register_ide_stream(struct pci_dev *pdev, struct pci_ide *ide)
{
	return sysfs_create_link(&tsm_subsys->dev.kobj, &pdev->dev.kobj,
				 ide->name);
}
EXPORT_SYMBOL_GPL(tsm_register_ide_stream);

void tsm_unregister_ide_stream(struct pci_ide *ide)
{
	sysfs_remove_link(&tsm_subsys->dev.kobj, ide->name);
}
EXPORT_SYMBOL_GPL(tsm_unregister_ide_stream);

static void tsm_release(struct device *dev)
{
	struct tsm_subsys *subsys = container_of(dev, typeof(*subsys), dev);

	kfree(subsys);
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
MODULE_DESCRIPTION("TEE Security Manager core");
