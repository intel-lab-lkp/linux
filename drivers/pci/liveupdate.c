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
 * :ref:`FLB <flb>` Data
 * =====================
 *
 * PCI device preservation across Live Update is built on top of the
 * :ref:`LUO <luo>` support for file preservation across kexec. Drivers are
 * expected to expose a file to represent a single PCI device and support
 * preservation of that file with ``ioctl(LIVEUPDATE_SESSION_PRESERVE_FD)``.
 * This allows userspace to control the preservation of devices and ensure
 * proper lifecycle management while a device is preserved. The first intended
 * use-case is preserving vfio-pci device files.
 *
 * The PCI core maintains its own state about what devices are being preserved
 * across Live Update using FLB data in LUO. Essentially, this allows the PCI
 * core to allocate struct pci_ser when the first device (file) is preserved
 * and free it when the last device (file) is unpreserved. After kexec, the
 * PCI core can fetch the struct pci_ser (which was constructed by the previous
 * kernel) from LUO at any time (e.g. during enumeration) so that it knows
 * which devices were preserved.
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

#define pr_fmt(fmt) "PCI: " KBUILD_BASENAME ": " fmt

#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/mm.h>
#include <linux/kho_block.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>

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
 * struct pci_flb_outgoing - Outgoing PCI FLB object
 * @ser: Pointer to the preserved struct pci_ser.
 * @block_set: The KHO block set holding the outgoing devices.
 *
 * This structure holds the runtime state for the outgoing PCI Live Update
 * state. It wraps the serialized pci_ser and the block_set used to manage
 * the serialized entries.
 */
struct pci_flb_outgoing {
	struct pci_ser *ser;
	struct kho_block_set block_set;
};

static int pci_flb_preserve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_outgoing *outgoing;
	struct pci_ser *ser;

	outgoing = kzalloc_obj(*outgoing);
	if (!outgoing)
		return -ENOMEM;

	ser = kho_alloc_preserve(sizeof(*ser));
	if (IS_ERR(ser)) {
		kfree(outgoing);
		return PTR_ERR(ser);
	}

	ser->nr_devices = 0;
	ser->devices = 0;

	outgoing->ser = ser;
	kho_block_set_init(&outgoing->block_set, sizeof(struct pci_dev_ser));

	args->obj = outgoing;
	args->data = virt_to_phys(ser);
	return 0;
}

static void pci_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_outgoing *outgoing = args->obj;

	WARN_ON(outgoing->ser->nr_devices);
	kho_block_set_destroy(&outgoing->block_set);
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

static struct pci_flb_outgoing *pci_liveupdate_flb_get_outgoing(void)
{
	struct pci_flb_outgoing *outgoing = NULL;
	int ret;

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&outgoing);
	if (ret)
		return ERR_PTR(ret);

	if (!outgoing)
		return ERR_PTR(-ENOENT);

	return outgoing;
}

static struct pci_dev_ser *pci_get_empty_or_append(struct pci_flb_outgoing *outgoing)
{
	struct pci_dev_ser *dev_ser, *found = NULL;
	struct kho_block_set_it it;
	int err;
	u32 count = 0;

	kho_block_set_it_init(&it, &outgoing->block_set);
	while ((dev_ser = kho_block_set_it_read_entry(&it))) {
		count++;
		if (dev_ser->refcount == 0 && !found)
			found = dev_ser;
	}

	if (found)
		return found;

	err = kho_block_set_grow(&outgoing->block_set, count + 1);
	if (err)
		return ERR_PTR(err);

	if (count == 0)
		kho_block_set_it_init(&it, &outgoing->block_set);

	dev_ser = kho_block_set_it_reserve_entry(&it);
	if (!dev_ser)
		return ERR_PTR(-ENOSPC);

	return dev_ser;
}

static void pci_liveupdate_unpreserve_device(struct pci_flb_outgoing *outgoing, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser = dev->liveupdate.outgoing;

	if (!dev_ser) {
		pci_warn(dev, "Cannot unpreserve device that is not preserved\n");
		return;
	}

	pci_info(dev, "Device will no longer be preserved across next Live Update\n");
	outgoing->ser->nr_devices--;
	memset(dev_ser, 0, sizeof(*dev_ser));
	dev->liveupdate.outgoing = NULL;
}

static int pci_liveupdate_preserve_device(struct pci_flb_outgoing *outgoing, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;

	if (dev->liveupdate.outgoing)
		return -EBUSY;

	dev_ser = pci_get_empty_or_append(outgoing);
	if (IS_ERR(dev_ser))
		return PTR_ERR(dev_ser);

	pci_info(dev, "Device will be preserved across next Live Update\n");
	outgoing->ser->nr_devices++;
	outgoing->ser->devices = kho_block_set_head_pa(&outgoing->block_set);

	dev_ser->domain = pci_domain_nr(dev->bus);
	dev_ser->bdf = pci_dev_id(dev);
	dev_ser->refcount = 1;

	dev->liveupdate.outgoing = dev_ser;
	return 0;
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
	struct pci_flb_outgoing *outgoing = NULL;

	if (dev->is_virtfn)
		return -EINVAL;

	guard(rwsem_write)(&pci_liveupdate.rwsem);

	outgoing = pci_liveupdate_flb_get_outgoing();
	if (IS_ERR(outgoing))
		return PTR_ERR(outgoing);

	return pci_liveupdate_preserve_device(outgoing, dev);
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
	struct pci_flb_outgoing *outgoing = NULL;

	guard(rwsem_write)(&pci_liveupdate.rwsem);

	outgoing = pci_liveupdate_flb_get_outgoing();
	if (IS_ERR(outgoing)) {
		pci_warn(dev, "Cannot unpreserve device without outgoing Live Update state\n");
		return;
	}

	pci_liveupdate_unpreserve_device(outgoing, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_unpreserve);

void pci_liveupdate_cleanup_device(struct pci_dev *dev)
{
	/*
	 * It should be safe to READ_ONCE() outside of the rwsem during cleanup
	 * since there should no longer be any references to @dev on the system.
	 *
	 * This should never happen in practice. Drivers should block removal
	 * while a device is preserved.
	 */
	if (READ_ONCE(dev->liveupdate.outgoing))
		pci_WARN(dev, 1, "Destroying outgoing-preserved device!\n");
}

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
