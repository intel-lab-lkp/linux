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
 * .. note::
 *    The support for preserving PCI devices across Live Update is currently
 *    *partial* and should be considered *experimental*. It should only be
 *    used by developers working on the implementation for the time being.
 *
 *    To enable the support, enable ``CONFIG_PCI_LIVEUPDATE``.
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

/**
 * struct pci_flb_outgoing - Outgoing PCI FLB object
 * @ser: The outgoing struct pci_ser for the next kernel.
 * @lock: Lock used to protect against changes to @ser.
 */
struct pci_flb_outgoing {
	struct pci_ser *ser;
	struct mutex lock;
};

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_outgoing *outgoing;
	struct pci_dev *dev = NULL;
	u32 max_nr_devices = 0;
	unsigned long size;

	outgoing = kmalloc_obj(*outgoing);
	if (!outgoing)
		return -ENOMEM;

	mutex_init(&outgoing->lock);

	/*
	 * Allocate enough space to preserve all of the devices that are
	 * currently present on the system. Extra padding can be added to this
	 * in the future to increase the chances that there is enough room to
	 * preserve devices that are not yet present on the system (e.g. VFs,
	 * hot-plugged devices).
	 */
	for_each_pci_dev(dev)
		max_nr_devices++;

	size = struct_size_t(struct pci_ser, devices, max_nr_devices);

	outgoing->ser = kho_alloc_preserve(size);
	if (IS_ERR(outgoing->ser)) {
		kfree(outgoing);
		return PTR_ERR(outgoing->ser);
	}

	pr_debug("Preserved struct pci_ser with room for %u devices\n",
		 max_nr_devices);

	outgoing->ser->max_nr_devices = max_nr_devices;
	outgoing->ser->nr_devices = 0;

	args->obj = outgoing;
	args->data = virt_to_phys(outgoing->ser);
	return 0;
}

static void pci_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_outgoing *outgoing = args->obj;

	WARN_ON_ONCE(outgoing->ser->nr_devices);
	kho_unpreserve_free(outgoing->ser);
	kfree(outgoing);

	pr_debug("Unpreserved struct pci_ser\n");
}

static int pci_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	args->obj = phys_to_virt(args->data);
	return 0;
}

static void pci_flb_finish(struct liveupdate_flb_op_args *args)
{
	kho_restore_free(args->obj);
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

/**
 * pci_liveupdate_preserve() - Preserve a PCI device across Live Update
 * @dev: The PCI device to preserve.
 *
 * pci_liveupdate_preserve() notifies the PCI core that a PCI device should be
 * preserved across the next Live Update. Drivers must call
 * pci_liveupdate_preserve() from their struct liveupdate_file_handler
 * preserve() callback to ensure the outgoing struct pci_ser is allocated.
 *
 * Returns: 0 on success, <0 on failure.
 */
int pci_liveupdate_preserve(struct pci_dev *dev)
{
	struct pci_flb_outgoing *outgoing = NULL;
	struct pci_ser *ser;
	int i, ret;

	if (dev->is_virtfn)
		return -EINVAL;

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&outgoing);
	if (ret)
		return ret;

	if (!outgoing)
		return -ENOENT;

	guard(mutex)(&outgoing->lock);
	ser = outgoing->ser;

	guard(write_lock)(&dev->liveupdate.lock);

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
EXPORT_SYMBOL_GPL(pci_liveupdate_preserve);

/**
 * pci_liveupdate_unpreserve() - Cancel preservation of a PCI device
 * @dev: The PCI device to preserve.
 *
 * pci_liveupdate_unpreserve() notifies the PCI core that a PCI device should no
 * longer be preserved across the next Live Update. Drivers must call
 * pci_liveupdate_unpreserve() from their struct liveupdate_file_handler
 * unpreserve() callback to ensure the outgoing struct pci_ser is allocated.
 */
void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
	struct pci_flb_outgoing *outgoing = NULL;
	struct pci_dev_ser *dev_ser;
	struct pci_ser *ser;
	int ret;

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&outgoing);

	if (ret || !outgoing) {
		pci_warn(dev, "Cannot unpreserve device without outgoing Live Update state\n");
		return;
	}

	guard(mutex)(&outgoing->lock);
	ser = outgoing->ser;

	guard(write_lock)(&dev->liveupdate.lock);

	dev_ser = dev->liveupdate.outgoing;
	if (!dev_ser) {
		pci_warn(dev, "Cannot unpreserve device that is not preserved\n");
		return;
	}

	pci_info(dev, "Device will no longer be preserved across next Live Update\n");
	ser->nr_devices--;
	memset(dev_ser, 0, sizeof(*dev_ser));
	dev->liveupdate.outgoing = NULL;
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unpreserve);

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
