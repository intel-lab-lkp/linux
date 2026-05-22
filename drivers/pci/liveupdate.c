// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

/**
 * DOC: PCI Live Update
 *
 * The PCI subsystem participates in the Live Update process to enable drivers
 * to preserve their PCI devices across kexec.
 *
 * File-Lifecycle-Bound (FLB) Data
 * ===============================
 *
 * PCI device preservation across Live Update is built on top of the Live Update
 * Orchestrator's (LUO) support for file preservation across kexec. Drivers
 * are expected to expose a file to represent a single PCI device and support
 * preservation of that file with ``ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)``.
 * This allows userspace to control the preservation of devices and ensure
 * proper lifecycle management while a device is preserved. The first intended
 * use-case is preserving vfio-pci device files.
 *
 * The PCI core maintains its own state about what devices are being preserved
 * across Live Update using a feature called File-Lifecycle-Bound (FLB) data in
 * LUO.  Essentially, this allows the PCI core to allocate struct pci_ser when
 * the first device (file) is preserved and free it when the last device (file)
 * is unpreserved. After kexec, the PCI core can fetch the struct pci_ser (which
 * was constructed by the previous kernel) from LUO at any time (e.g. during
 * enumeration) so that it knows which devices were preserved.
 *
 * To enable the PCI core to be notified whenever a file representing a device
 * is preserved, drivers must register their struct liveupdate_file_handler with
 * the PCI core by using the following APIs:
 *
 *  * ``pci_liveupdate_register_flb(driver_file_handler)``
 *  * ``pci_liveupdate_unregister_flb(driver_file_handler)``
 *
 * Device Tracking
 * ===============
 *
 * Drivers must notify the PCI core when specific devices are preserved or
 * unpreserved with the following APIs:
 *
 *  * ``pci_liveupdate_preserve(pci_dev)``
 *  * ``pci_liveupdate_unpreserve(pci_dev)``
 *
 * This allows the PCI core to keep its FLB data (struct pci_ser) up to date
 * with the list of **outgoing** preserved devices for the next kernel.
 *
 * After kexec, whenever a device is enumerated, the PCI core will check if it
 * is an **incoming** preserved device (i.e. preserved by the previous kernel)
 * by checking the incoming FLB data (struct pci_ser).
 *
 * Drivers must notify the PCI core when an **incoming** device is done
 * participating in the incoming Live Update with the following API:
 *
 *  * ``pci_liveupdate_finish(pci_dev)``
 *
 * The PCI core does not enforce any ordering of ``pci_liveupdate_finish()`` and
 * ``pci_liveupdate_preserve()``. i.e. A PCI device can be **outgoing**
 * (preserved for next kernel) and **incoming** (preserved by previous kernel)
 * at the same time.
 *
 * Restrictions
 * ============
 *
 * The PCI core enforces the following restrictions on which devices can be
 * preserved. These may be relaxed in the future:
 *
 *  * The device cannot be a Virtual Function (VF).
 */

#define pr_fmt(fmt) "PCI: liveupdate: " fmt

#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "liveupdate.h"

/**
 * struct pci_liveupdate_global - Global state for PCI Live Update support
 * @rwsem: Reader/writer semaphore used to protect the incoming and outgoing
 *         FLBs, and the references to them in struct pci_dev.
 */
struct pci_liveupdate_global {
	struct rw_semaphore rwsem;
};

static struct pci_liveupdate_global pci_liveupdate = {
	.rwsem = __RWSEM_INITIALIZER(pci_liveupdate.rwsem),
};

/**
 * struct pci_flb_incoming - Incoming PCI FLB object
 * @ser: The incoming struct pci_ser from the previous kernel.
 * @xa: Xarray used to quickly lookup devices in @ser.
 */
struct pci_flb_incoming {
	struct pci_ser *ser;
	struct xarray xa;
};

static unsigned long pci_ser_xa_key(u32 domain, u16 bdf)
{
	return (unsigned long)domain << 16 | bdf;
}

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_dev *dev = NULL;
	u32 max_nr_devices = 0;
	struct pci_ser *ser;
	unsigned long size;

	/*
	 * Allocate enough space to preserve all devices that are currently
	 * present on the system. Extra padding can be added to this in the
	 * future to increase the chances that there is enough room to preserve
	 * devices that are not yet present on the system (e.g. VFs, hot-plugged
	 * devices).
	 */
	for_each_pci_dev(dev)
		max_nr_devices++;

	size = struct_size_t(struct pci_ser, devices, max_nr_devices);

	ser = kho_alloc_preserve(size);
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	pr_debug("Preserved struct pci_ser with room for %u devices\n",
		 max_nr_devices);

	ser->max_nr_devices = max_nr_devices;
	ser->nr_devices = 0;

	args->obj = ser;
	args->data = virt_to_phys(ser);
	return 0;
}

static void pci_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct pci_ser *ser = args->obj;

	WARN_ON(ser->nr_devices);
	kho_unpreserve_free(ser);
	pr_debug("Unpreserved struct pci_ser\n");
}

static int pci_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	struct pci_ser *ser = phys_to_virt(args->data);
	struct pci_flb_incoming *incoming;
	int ret = -ENOMEM;
	u32 i;

	incoming = kmalloc_obj(*incoming);
	if (!incoming)
		goto err_restore_free;

	incoming->ser = ser;
	xa_init(&incoming->xa);

	for (i = 0; i < incoming->ser->max_nr_devices; i++) {
		struct pci_dev_ser *dev_ser = &incoming->ser->devices[i];
		unsigned long key;

		if (!dev_ser->refcount)
			continue;

		key = pci_ser_xa_key(dev_ser->domain, dev_ser->bdf);
		ret = xa_insert(&incoming->xa, key, dev_ser, GFP_KERNEL);
		if (ret)
			goto err_xa_destroy;
	}

	args->obj = incoming;
	return 0;

err_xa_destroy:
	xa_destroy(&incoming->xa);
	kfree(incoming);
err_restore_free:
	kho_restore_free(ser);
	return ret;
}

static void pci_flb_finish(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_incoming *incoming = args->obj;

	xa_destroy(&incoming->xa);
	kho_restore_free(incoming->ser);
	kfree(incoming);
}

static struct liveupdate_flb_ops pci_liveupdate_flb_ops = {
	.preserve = pci_flb_preserve,
	.unpreserve = pci_flb_unpreserve,
	.retrieve = pci_flb_retrieve,
	.finish = pci_flb_finish,
	.owner = THIS_MODULE,
};

static struct liveupdate_flb pci_liveupdate_flb = {
	.ops = &pci_liveupdate_flb_ops,
	.compatible = PCI_LUO_FLB_COMPATIBLE,
};

static struct pci_ser *pci_liveupdate_flb_get_outgoing(void)
{
	struct pci_ser *ser = NULL;
	int ret;

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&ser);
	if (ret)
		return ERR_PTR(ret);

	if (!ser)
		return ERR_PTR(-ENOENT);

	return ser;
}

static void pci_liveupdate_unpreserve_device(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser = dev->liveupdate.outgoing;

	if (!dev_ser) {
		pci_warn(dev, "Cannot unpreserve device that is not preserved\n");
		return;
	}

	pci_info(dev, "Device will no longer be preserved across next Live Update\n");
	ser->nr_devices--;
	memset(dev_ser, 0, sizeof(*dev_ser));
	dev->liveupdate.outgoing = NULL;
}

static int pci_liveupdate_preserve_device(struct pci_ser *ser, struct pci_dev *dev)
{
	int i;

	if (dev->liveupdate.outgoing)
		return -EBUSY;

	if (ser->nr_devices == ser->max_nr_devices)
		return -ENOSPC;

	for (i = 0; i < ser->max_nr_devices; i++) {
		/*
		 * Start searching at index ser->nr_devices. This should result
		 * in a constant time search under expected conditions (devices
		 * are not getting unpreserved).
		 */
		int index = (ser->nr_devices + i) % ser->max_nr_devices;
		struct pci_dev_ser *dev_ser = &ser->devices[index];

		if (dev_ser->refcount)
			continue;

		pci_info(dev, "Device will be preserved across next Live Update\n");
		ser->nr_devices++;

		dev_ser->domain = pci_domain_nr(dev->bus);
		dev_ser->bdf = pci_dev_id(dev);
		dev_ser->refcount = 1;

		dev->liveupdate.outgoing = dev_ser;
		return 0;
	}

	return -ENOSPC;
}

/**
 * pci_liveupdate_preserve() - Preserve a PCI device across Live Update
 * @dev: The PCI device to preserve.
 *
 * pci_liveupdate_preserve() notifies the PCI core that a PCI device should be
 * preserved across the next Live Update. Drivers are expected to call
 * pci_liveupdate_preserve() from their struct liveupdate_file_handler
 * preserve() callback to ensure the outgoing struct pci_ser is already set up.
 *
 * Returns: 0 on success, <0 on failure.
 */
int pci_liveupdate_preserve(struct pci_dev *dev)
{
	struct pci_ser *ser = NULL;

	if (dev->is_virtfn)
		return -EINVAL;

	guard(rwsem_write)(&pci_liveupdate.rwsem);

	ser = pci_liveupdate_flb_get_outgoing();
	if (IS_ERR(ser))
		return PTR_ERR(ser);

	return pci_liveupdate_preserve_device(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_preserve);

/**
 * pci_liveupdate_unpreserve() - Cancel preservation of a PCI device
 * @dev: The PCI device to unpreserve.
 *
 * pci_liveupdate_unpreserve() notifies the PCI core that a PCI device should no
 * longer be preserved across the next Live Update. Drivers are expected to call
 * pci_liveupdate_unpreserve() from their struct liveupdate_file_handler
 * unpreserve() callback to ensure the outgoing struct pci_ser is already set
 * up.
 */
void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
	struct pci_ser *ser = NULL;

	guard(rwsem_write)(&pci_liveupdate.rwsem);

	ser = pci_liveupdate_flb_get_outgoing();
	if (IS_ERR(ser)) {
		pci_warn(dev, "Cannot unpreserve device without outgoing Live Update state\n");
		return;
	}

	pci_liveupdate_unpreserve_device(ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unpreserve);

static struct pci_flb_incoming *pci_liveupdate_flb_get_incoming(void)
{
	struct pci_flb_incoming *incoming = NULL;
	int ret;

	ret = liveupdate_flb_get_incoming(&pci_liveupdate_flb, (void **)&incoming);

	/* Live Update is not enabled. */
	if (ret == -EOPNOTSUPP)
		return NULL;

	/* Live Update is enabled, but there is no incoming FLB data. */
	if (ret == -ENODATA)
		return NULL;

	/*
	 * Live Update is enabled and there is incoming FLB data, but none of it
	 * matches pci_liveupdate_flb.compatible.
	 *
	 * This could mean that no PCI FLB data was passed by the previous
	 * kernel, but it could also mean the previous kernel used a different
	 * compatibility string (i.e. a different ABI).
	 */
	if (ret == -ENOENT) {
		pr_info_once("No incoming FLB matched %s\n", pci_liveupdate_flb.compatible);
		return NULL;
	}

	/*
	 * There is incoming FLB data that matches pci_liveupdate_flb.compatible
	 * but it cannot be retrieved.
	 */
	if (ret) {
		WARN_ONCE(ret, "Failed to retrieve incoming FLB data\n");
		return NULL;
	}

	return incoming;
}

static void pci_liveupdate_flb_put_incoming(void)
{
	liveupdate_flb_put_incoming(&pci_liveupdate_flb);
}

void pci_liveupdate_setup_device(struct pci_dev *dev)
{
	struct pci_flb_incoming *incoming;
	struct pci_dev_ser *dev_ser;
	unsigned long key;

	guard(rwsem_write)(&pci_liveupdate.rwsem);

	incoming = pci_liveupdate_flb_get_incoming();
	if (!incoming)
		return;

	key = pci_ser_xa_key(pci_domain_nr(dev->bus), pci_dev_id(dev));
	dev_ser = xa_load(&incoming->xa, key);

	/* This device was not preserved across Live Update */
	if (!dev_ser) {
		pci_liveupdate_flb_put_incoming();
		return;
	}

	/*
	 * This device was preserved, but has already been probed and gone
	 * through pci_liveupdate_finish(). This can happen if PCI core probes
	 * the same device multiple times, e.g. due to hotplug.
	 */
	if (!dev_ser->refcount) {
		pci_liveupdate_flb_put_incoming();
		return;
	}

	pci_info(dev, "Device was preserved by previous kernel across Live Update\n");
	dev->liveupdate.incoming = dev_ser;

	/*
	 * Hold the ref on the incoming FLB until pci_liveupdate_finish() so
	 * that dev->liveupdate.incoming does not get freed while it is in use.
	 */
}

void pci_liveupdate_cleanup_device(struct pci_dev *dev)
{
	/*
	 * It should be safe to READ_ONCE() outside of the rwsem during cleanup
	 * since there should no longer be any references to @dev on the system.
	 */
	if (READ_ONCE(dev->liveupdate.outgoing)) {
		pci_WARN(dev, 1, "Destroying outgoing-preserved device!\n");
		pci_liveupdate_unpreserve(dev);
	}

	if (READ_ONCE(dev->liveupdate.incoming)) {
		pci_WARN(dev, 1, "Destroying incoming-preserved device!\n");
		pci_liveupdate_finish(dev);
	}
}

static void pci_liveupdate_finish_device(struct pci_ser *ser, struct pci_dev *dev)
{
	if (!dev->liveupdate.incoming) {
		pci_warn(dev, "Cannot finish preserving an unpreserved device\n");
		return;
	}

	if (dev->liveupdate.incoming->refcount != 1) {
		pci_WARN(dev, 1, "Preserved device has a corrupted refcount!\n");
		return;
	}

	/*
	 * Drop the refcount so this device does not get treated as an incoming
	 * device again, e.g. in case pci_liveupdate_setup_device() gets called
	 * again because the device is hot-plugged.
	 */
	dev->liveupdate.incoming->refcount = 0;

	pci_info(dev, "Device is finished participating in Live Update\n");
	dev->liveupdate.incoming = NULL;
	ser->nr_devices--;
	pci_liveupdate_flb_put_incoming();
}

/**
 * pci_liveupdate_finish() - Finish the preservation of a PCI device
 * @dev: The PCI device
 *
 * pci_liveupdate_finish() notifies the PCI core that a PCI device that was
 * preserved across the previous Live Update has finished participating in Live
 * Update. Drivers must call pci_liveupdate_finish() from their struct
 * liveupdate_file_handler finish() callback to ensure the incoming struct
 * pci_ser is allocated.
 */
void pci_liveupdate_finish(struct pci_dev *dev)
{
	struct pci_flb_incoming *incoming;

	guard(rwsem_write)(&pci_liveupdate.rwsem);

	incoming = pci_liveupdate_flb_get_incoming();
	if (!incoming) {
		pci_warn(dev, "Cannot finish preserving device without incoming FLB\n");
		return;
	}

	pci_liveupdate_finish_device(incoming->ser, dev);
	pci_liveupdate_flb_put_incoming();
}
EXPORT_SYMBOL_GPL(pci_liveupdate_finish);

/**
 * pci_liveupdate_is_incoming() - Check if a device is incoming-preserved
 * @dev: The PCI device to check
 *
 * Check if a device was preserved across Live Update by the previous kernel,
 * i.e. the device is incoming-preserved. Note that a device is only considered
 * incoming-preserved prior to pci_liveupdate_finish(). It is up to drivers to
 * synchronize usage of pci_liveupdate_is_incoming() with their own call to
 * pci_liveupdate_finish() to avoid acting on stale data.
 *
 * Returns: True if the device is incoming-preserved, false otherwise.
 */
bool pci_liveupdate_is_incoming(struct pci_dev *dev)
{
	guard(rwsem_read)(&pci_liveupdate.rwsem);
	return dev->liveupdate.incoming;
}
EXPORT_SYMBOL_GPL(pci_liveupdate_is_incoming);

/**
 * pci_liveupdate_register_flb() - Register a file handler with the PCI core
 * @fh: The file handler to register.
 *
 * Drivers should call pci_liveupdate_register_flb() to register their
 * struct liveupdate_file_handler with the PCI core. This enables the PCI core
 * to allocate its outgoing struct pci_ser whenever the first device is
 * preserved, and free it when the last device is unpreserved.
 *
 * Return: 0 on success, <0 on failure.
 */
int pci_liveupdate_register_flb(struct liveupdate_file_handler *fh)
{
	pr_debug("Registering file handler \"%s\"\n", fh->compatible);
	return liveupdate_register_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_register_flb);

/**
 * pci_liveupdate_unregister_flb() - Unregister a file handler with the PCI core
 * @fh: The file handler to unregister.
 */
void pci_liveupdate_unregister_flb(struct liveupdate_file_handler *fh)
{
	pr_debug("Unregistering file handler \"%s\"\n", fh->compatible);
	liveupdate_unregister_flb(fh, &pci_liveupdate_flb);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unregister_flb);
