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
 */

#define pr_fmt(fmt) "PCI: liveupdate: " fmt

#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/pci.h>

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_dev *dev = NULL;
	u32 max_nr_devices = 0;
	struct pci_ser *ser;
	unsigned long size;

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

	WARN_ON_ONCE(ser->nr_devices);
	kho_unpreserve_free(ser);

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
