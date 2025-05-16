// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tsm.h>
#include <linux/rwsem.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/cleanup.h>
#include <linux/pci-tsm.h>

static DECLARE_RWSEM(tsm_core_rwsem);
static struct class *tsm_class;
static struct tsm_core_dev {
	struct device dev;
	const struct pci_tsm_ops *pci_ops;
} *tsm_core;

static struct tsm_core_dev *
alloc_tsm_core(struct device *parent, const struct attribute_group **groups)
{
	struct tsm_core_dev *core = kzalloc(sizeof(*core), GFP_KERNEL);
	struct device *dev;

	if (!core)
		return ERR_PTR(-ENOMEM);
	dev = &core->dev;
	dev->parent = parent;
	dev->groups = groups;
	dev->class = tsm_class;
	device_initialize(dev);
	return core;
}

static void put_tsm_core(struct tsm_core_dev *core)
{
	put_device(&core->dev);
}

DEFINE_FREE(put_tsm_core, struct tsm_core_dev *,
	    if (!IS_ERR_OR_NULL(_T)) put_tsm_core(_T))
struct tsm_core_dev *tsm_register(struct device *parent,
				  const struct attribute_group **groups,
				  const struct pci_tsm_ops *pci_ops)
{
	struct device *dev;
	int rc;

	guard(rwsem_write)(&tsm_core_rwsem);
	if (tsm_core) {
		dev_warn(parent, "failed to register: %s already registered\n",
			 dev_name(tsm_core->dev.parent));
		return ERR_PTR(-EBUSY);
	}

	struct tsm_core_dev *core __free(put_tsm_core) =
		alloc_tsm_core(parent, groups);
	if (IS_ERR(core))
		return core;

	dev = &core->dev;
	rc = dev_set_name(dev, "tsm0");
	if (rc)
		return ERR_PTR(rc);

	rc = pci_tsm_core_register(pci_ops, NULL);
	if (rc) {
		dev_err(parent, "PCI initialization failure: %pe\n",
			ERR_PTR(rc));
		return ERR_PTR(rc);
	}

	rc = device_add(dev);
	if (rc) {
		pci_tsm_core_unregister(pci_ops);
		return ERR_PTR(rc);
	}

	core->pci_ops = pci_ops;
	tsm_core = no_free_ptr(core);

	return tsm_core;
}
EXPORT_SYMBOL_GPL(tsm_register);

void tsm_unregister(struct tsm_core_dev *core)
{
	guard(rwsem_write)(&tsm_core_rwsem);
	if (!tsm_core || core != tsm_core) {
		pr_warn("failed to unregister, not currently registered\n");
		return;
	}

	pci_tsm_core_unregister(core->pci_ops);
	device_unregister(&core->dev);

	tsm_core = NULL;
}
EXPORT_SYMBOL_GPL(tsm_unregister);

static void tsm_release(struct device *dev)
{
	struct tsm_core_dev *core = container_of(dev, typeof(*core), dev);

	kfree(core);
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
