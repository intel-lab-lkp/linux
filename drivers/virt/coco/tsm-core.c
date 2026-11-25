// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2024-2025 Intel Corporation. All rights reserved. */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/tsm.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/cleanup.h>
#include <linux/jiffies.h>
#include <linux/pci-tsm.h>
#include <linux/pid.h>
#include <linux/sched.h>

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
 * tsm_trylock_registration() - protect an operation against TSM unregister
 * @tsm_dev: TSM whose allocation is protected by the caller
 *
 * Never wait for the read lock: unregister holds the write lock while
 * draining users. * tsm_get() takes a user reference before releasing the
 * read lock.
 *
 * Return: true with the read lock held, or false if unregister prevents use.
 */
bool tsm_trylock_registration(struct tsm_dev *tsm_dev)
{
	if (!down_read_trylock(&tsm_dev->registration_lock))
		return false;

	if (tsm_dev->unregistering) {
		up_read(&tsm_dev->registration_lock);
		return false;
	}

	return true;
}

void tsm_unlock_registration(struct tsm_dev *tsm_dev)
{
	up_read(&tsm_dev->registration_lock);
}

static void tsm_put_user(struct tsm_dev *tsm_dev)
{
	if (refcount_dec_and_test(&tsm_dev->users))
		complete_all(&tsm_dev->users_drain_complete);
}

/**
 * tsm_get() - acquire a user reference to a TSM found by a bus lookup
 * @tsm_dev: TSM whose lifetime is protected by the caller
 *
 * The caller must protect @tsm_dev against removal until this returns.
 * A successful call takes a device reference, pins the backend module and
 * adds a user that tsm_unregister() will wait for. Release it with
 * tsm_put_device().
 *
 * Return: @tsm_dev on success, or ERR_PTR(-ENODEV) if the TSM is unregistering
 * or the backend module cannot be pinned.
 */
struct tsm_dev *tsm_get(struct tsm_dev *tsm_dev)
{
	if (!tsm_trylock_registration(tsm_dev))
		return ERR_PTR(-ENODEV);

	/* Keep the backend code and operation tables loaded while in use. */
	if (!try_module_get(tsm_dev->pci_ops->owner)) {
		tsm_unlock_registration(tsm_dev);
		return ERR_PTR(-ENODEV);
	}

	/*
	 * Keep tsm_dev allocated through tsm_put_device(), including after
	 * dropping the user reference wakes tsm_unregister().
	 */
	get_device(&tsm_dev->dev);
	/*
	 * Make explicit unregister wait before tearing down backend
	 * resources.
	 */
	refcount_inc(&tsm_dev->users);
	tsm_unlock_registration(tsm_dev);
	return tsm_dev;
}

/**
 * tsm_get_device() - Pin the TSM attached to a device
 * @dev: device whose TSM is to be pinned
 *
 * A successful lookup takes a TSM device reference and pins the backend
 * module. It also adds a user that tsm_unregister() will wait for. Release
 * these references with tsm_put_device().
 * Return: NULL if no TSM is attached, an error if the module
 * cannot be pinned or the TSM is unregistering, or the TSM device associated
 * with the pinned backend.
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
	struct module *owner;

	if (!tsm_dev)
		return;

	owner = tsm_dev->pci_ops->owner;
	tsm_put_user(tsm_dev);
	put_device(&tsm_dev->dev);
	module_put(owner);
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
	init_rwsem(&tsm_dev->registration_lock);
	refcount_set(&tsm_dev->users, 1);
	init_completion(&tsm_dev->users_drain_complete);
	dev = &tsm_dev->dev;
	dev->parent = parent;
	dev->class = &tsm_class;
	device_initialize(dev);

	return no_free_ptr(tsm_dev);
}

struct tsm_dev *tsm_register(struct device *parent, struct pci_tsm_ops *pci_ops)
{
	struct tsm_dev *tsm_dev __free(put_tsm_dev) = alloc_tsm_dev(parent);
	struct device *dev;
	int rc;

	if (!pci_ops)
		return ERR_PTR(-EINVAL);

	if (IS_ERR(tsm_dev))
		return tsm_dev;

	dev = &tsm_dev->dev;
	rc = dev_set_name(dev, "tsm%d", tsm_dev->id);
	if (rc)
		return ERR_PTR(rc);

	/* Complete PCI registration before exposing the TSM to class lookup. */
	tsm_dev->pci_ops = pci_ops;
	rc = pci_tsm_register(tsm_dev);
	if (rc) {
		dev_err(parent, "PCI/TSM registration failure: %d\n", rc);
		return ERR_PTR(rc);
	}

	rc = device_add(dev);
	if (rc) {
		pci_tsm_unregister(tsm_dev);
		return ERR_PTR(rc);
	}

	/* Notify TSM userspace that PCI/TSM operations are now possible. */
	kobject_uevent(&dev->kobj, KOBJ_CHANGE);

	return no_free_ptr(tsm_dev);
}
EXPORT_SYMBOL_GPL(tsm_register);

static void tsm_wait_for_users(struct tsm_dev *tsm_dev)
{
	struct completion *drain_complete = &tsm_dev->users_drain_complete;
	bool interrupted = false;
	long rc = try_wait_for_completion(drain_complete);

	while (rc <= 0) {
		if (interrupted) {
			rc = wait_for_completion_timeout(drain_complete, HZ * 10);
		} else {
			rc = wait_for_completion_interruptible_timeout(
				drain_complete, HZ * 10);
			if (rc < 0) {
				interrupted = true;
				dev_warn(&tsm_dev->dev,
					 "Task \"%s\" (%d) unregistering TSM; release virtual IOMMUs and other TSM users\n",
					 current->comm, task_pid_nr(current));
			}
		}

		if (!rc)
			dev_warn(&tsm_dev->dev, "TSM unregister blocked; release virtual IOMMUs and other TSM users\n");
	}
}

/**
 * tsm_unregister() - unregister a TSM after draining its users
 * @tsm_dev: TSM to unregister
 *
 * Blocks new users and waits for references acquired by tsm_get_device(),
 * including vIOMMUs without vdevices, and pending DSM removals before tearing
 * down PCI connections. The wait cannot be aborted by a signal; userspace
 * must release the vIOMMUs and vdevices to let it finish.
 *
 * Return: 0 on success, or -ENODEV if unregistration has already started.
 */
int tsm_unregister(struct tsm_dev *tsm_dev)
{
	down_write(&tsm_dev->registration_lock);
	if (tsm_dev->unregistering) {
		up_write(&tsm_dev->registration_lock);
		return -ENODEV;
	}

	tsm_dev->unregistering = true;

	device_del(&tsm_dev->dev);
	/* No new user references can be acquired while the write lock is held. */
	tsm_put_user(tsm_dev);
	tsm_wait_for_users(tsm_dev);

	if (IS_ENABLED(CONFIG_PCI_TSM))
		pci_tsm_unregister(tsm_dev);

	up_write(&tsm_dev->registration_lock);
	put_device(&tsm_dev->dev);
	return 0;
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
subsys_initcall(tsm_init)

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TEE Security Manager Class Device");
