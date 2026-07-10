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
 *
 *  * The device cannot require device-specific quirks to enable Access
 *    Control Services (ACS).
 *
 * Driver Binding
 * ==============
 *
 * In the outgoing kernel, it is the driver's responsibility to ensure that it
 * does not release a device between pci_liveupdate_preserve() and
 * pci_liveupdate_unpreserve().
 *
 * In the incoming kernel, it is the driver's responsibility to ensure that it
 * does not release a preserved device between probe() and
 * pci_liveupdate_finish().
 *
 * It is the user's responsibility to ensure that incoming preserved devices are
 * bound to the correct driver. i.e. The PCI core does not protect against a
 * device getting preserved by driver A in the outgoing kernel and then getting
 * bound to driver B in the incoming kernel. This may change in the future.
 *
 * BDF Stability
 * =============
 *
 * The PCI core guarantees that preserved devices can be identified by the same
 * bus, device, and function numbers for as long as they are preserved
 * (including across kexec). To accomplish this, the PCI core always inherits
 * the secondary and subordinate bus numbers assigned to bridges during scanning
 * if any device is preserved. This is true even on architectures that always
 * assign new bus numbers during scanning. The kernel assumes the previous
 * kernel established a sane bus topology across kexec.
 *
 * If a misconfigured or unconfigured bridge is encountered during enumeration
 * while there are preserved devices, its secondary and subordinate bus numbers
 * will be cleared and devices below it will not be enumerated.
 *
 * PCI-to-PCI Bridges
 * ==================
 *
 * Any PCI-to-PCI bridges upstream of a preserved device are automatically
 * preserved when the device is preserved. The PCI core keeps track of the
 * number of downstream devices that are preserved under a bridge so that the
 * bridge is only unpreserved once all downstream devices are unpreserved.
 *
 * This enables the PCI core and any drivers bound to the bridge to participate
 * in the Live Update so that preserved endpoints can continue issuing memory
 * transactions during the Live Update.
 *
 * Handling Preserved Devices
 * ==========================
 *
 * The PCI core treats preserved devices differently than non-preserved devices.
 * This section enumerates those differences.
 *
 *  * The PCI core inherits all ACS flags enabled on incoming preserved devices
 *    rather than assigning new ones. This ensures that TLPs are routed the same
 *    way after Live Update and ensures that IOMMU groups do not change. Note
 *    that a device will use its inherited ACS flags for the lifetime of its
 *    struct pci_dev (i.e. even after pci_liveupdate_finish()).
 *
 *  * The PCI core inherits ARI Forwarding Enable on all bridges with downstream
 *    preserved devices to ensure that all preserved devices on the bridge's
 *    secondary bus are addressable after the Live Update.
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
#include "pci.h"

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

/**
 * struct pci_flb_incoming - Incoming PCI FLB object
 * @ser: The incoming struct pci_ser from the previous kernel.
 * @xa: Xarray used to quickly lookup devices in @ser.
 * @block_set: The KHO block set holding the incoming devices.
 *
 * This structure holds the runtime state for the incoming PCI Live Update
 * state. It wraps the serialized pci_ser, the block_set used to restore
 * the serialized entries, and an xarray for fast lookups.
 */
struct pci_flb_incoming {
	struct pci_ser *ser;
	struct xarray xa;
	struct kho_block_set block_set;
};

static unsigned long pci_ser_xa_key(u32 domain, u16 bdf)
{
	return (unsigned long)domain << 16 | bdf;
}
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
	struct pci_ser *ser = phys_to_virt(args->data);
	struct pci_flb_incoming *incoming;
	struct pci_dev_ser *dev_ser;
	struct kho_block_set_it it;
	int ret = -ENOMEM;

	incoming = kzalloc_obj(*incoming);
	if (!incoming)
		goto err_restore_free;

	incoming->ser = ser;
	xa_init(&incoming->xa);

	kho_block_set_init(&incoming->block_set, sizeof(struct pci_dev_ser));
	ret = kho_block_set_restore(&incoming->block_set, ser->devices);
	if (ret)
		goto err_free_incoming;

	kho_block_set_it_init(&it, &incoming->block_set);
	while ((dev_ser = kho_block_set_it_read_entry(&it))) {
		unsigned long key;

		if (!dev_ser->refcount)
			continue;

		key = pci_ser_xa_key(dev_ser->domain, dev_ser->bdf);
		ret = xa_insert(&incoming->xa, key, dev_ser, GFP_KERNEL);
		if (ret)
			goto err_block_set_destroy;
	}

	args->obj = incoming;
	return 0;

err_block_set_destroy:
	kho_block_set_destroy(&incoming->block_set);
err_free_incoming:
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
	kho_block_set_destroy(&incoming->block_set);
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

static int pci_liveupdate_unpreserve_device(struct pci_flb_outgoing *outgoing, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser = dev->liveupdate.outgoing;

	if (dev->liveupdate.frozen) {
		pci_warn(dev, "Cannot unpreserve device after it is frozen!\n");
		return -EINVAL;
	}

	if (!dev_ser) {
		pci_warn(dev, "Cannot unpreserve device that is not preserved\n");
		return -EINVAL;
	}

	if (!dev_ser->refcount) {
		pci_WARN(dev, 1, "Preserved device has a 0 refcount!\n");
		return -EINVAL;
	}

	if (--dev_ser->refcount)
		return 0;

	pci_info(dev, "Device will no longer be preserved across next Live Update\n");
	outgoing->ser->nr_devices--;
	memset(dev_ser, 0, sizeof(*dev_ser));
	dev->liveupdate.outgoing = NULL;
	return 0;
}

static int pci_liveupdate_preserve_device_again(struct pci_dev *dev)
{
	if (!dev->liveupdate.outgoing->refcount) {
		pci_WARN(dev, 1, "Preserved device with 0 refcount!\n");
		return -EINVAL;
	}

	/*
	 * Endpoint devices should not be preserved more than once. Bridges are
	 * preserved once for every downstream device that is preserved.
	 */
	if (!dev->subordinate)
		return -EBUSY;

	dev->liveupdate.outgoing->refcount++;
	return 0;
}

static int __pci_liveupdate_preserve_device(struct pci_flb_outgoing *outgoing, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;

	/*
	 * Do not preserve devices that rely on device-specific ACS equivalents
	 * (for now) since that would complicate keeping ACS constant across
	 * Live Update.
	 */
	if (pci_need_dev_specific_enable_acs(dev)) {
		pci_warn(dev, "Refusing to preserve device that relies on ACS quirks\n");
		return -EINVAL;
	}

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

static int pci_liveupdate_preserve_device(struct pci_flb_outgoing *outgoing, struct pci_dev *dev)
{
	if (dev->liveupdate.frozen) {
		pci_warn(dev, "Cannot preserve device after it is frozen!\n");
		return -EINVAL;
	}

	if (dev->liveupdate.outgoing)
		return pci_liveupdate_preserve_device_again(dev);

	return __pci_liveupdate_preserve_device(outgoing, dev);
}

#define for_each_pci_dev_in_path(_d, _start, _end) \
	for ((_d) = (_start); (_d) != (_end); (_d) = (_d)->bus->self)

static void __pci_liveupdate_unpreserve_path(struct pci_flb_outgoing *outgoing,
					     struct pci_dev *start,
					     struct pci_dev *end)
{
	struct pci_dev *dev;

	for_each_pci_dev_in_path(dev, start, end) {
		if (pci_liveupdate_unpreserve_device(outgoing, dev))
			return;
	}
}

static void pci_liveupdate_unpreserve_path(struct pci_flb_outgoing *outgoing,
					   struct pci_dev *start)
{
	__pci_liveupdate_unpreserve_path(outgoing, start, /*end=*/NULL);
}

static int pci_liveupdate_preserve_path(struct pci_flb_outgoing *outgoing,
					struct pci_dev *start)
{
	struct pci_dev *dev;
	int ret;

	for_each_pci_dev_in_path(dev, start, NULL) {
		ret = pci_liveupdate_preserve_device(outgoing, dev);
		if (ret) {
			__pci_liveupdate_unpreserve_path(outgoing, start, dev);
			return ret;
		}
	}

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
 * pci_liveupdate_preserve() automatically preserves all bridges upstream of
 * @dev.
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

	return pci_liveupdate_preserve_path(outgoing, dev);
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
 *
 * pci_liveupdate_unpreserve() automatically unpreserves all bridges upstream of
 * @dev.
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

	pci_liveupdate_unpreserve_path(outgoing, dev);
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
	if (ret)
		panic("Failed to retrieve incoming FLB data (%d)\n", ret);

	return incoming;
}

static void pci_liveupdate_flb_put_incoming(void)
{
	liveupdate_flb_put_incoming(&pci_liveupdate_flb);
}

/**
 * pci_liveupdate_scan_bridge_begin() - Determine if a bridge should inherit bus numbers
 * @bus: The parent bus of the bridge.
 * @dev: The PCI bridge device.
 * @pass: The scan pass (0 for first pass, 1 for second pass).
 *
 * This function is called by the PCI core when it begins scanning a bridge.
 * It determines whether the bridge should inherit the secondary and subordinate
 * bus numbers assigned to it by the previous kernel. This is necessary to
 * keep bus numbers constant for preserved devices downstream of the bridge.
 *
 * Return: True if bus numbers should be inherited, false otherwise.
 */
bool pci_liveupdate_scan_bridge_begin(struct pci_bus *bus, struct pci_dev *dev,
				      int pass)
{
	struct pci_dev *parent = bus->self;

	/*
	 * On the second pass, reuse the value that was set on the first pass
	 * so that the passes are consistent with one another.
	 */
	if (pass)
		return dev->liveupdate.inherit_buses;

	/*
	 * If the parent bridge is being forced to inherit its bus numbers
	 * during this scan then this bridge must as well, otherwise the PCI
	 * core could expand this bridge's reservation beyond its parent (which
	 * cannot expand).
	 */
	if (parent && parent->liveupdate.inherit_buses) {
		dev->liveupdate.inherit_buses = true;
		return true;
	}

	/*
	 * Otherwise, if there are any incoming preserved devices, force the
	 * bus numbers to be inherited to avoid changing the bus numbers
	 * assigned to those devices during enumeration.
	 *
	 * To keep things simple, inherit bus numbers on all bridges if any PCI
	 * devices are incoming, to ensure that no bridge's reservation is
	 * expanded to overlap with a preserved device downstream of a different
	 * bridge.
	 */
	scoped_guard(rwsem_read, &pci_liveupdate.rwsem) {
		struct pci_flb_incoming *incoming;

		incoming = pci_liveupdate_flb_get_incoming();
		if (!incoming) {
			dev->liveupdate.inherit_buses = false;
			return false;
		}

		/*
		 * It is safe to sample incoming->ser->nr_devices and then
		 * drop the rwsem since nr_devices will only decrease. Thus the
		 * only "race" is that the current scan will be overly
		 * conservative and force bus inheritance.
		 */
		dev->liveupdate.inherit_buses = !!incoming->ser->nr_devices;
		pci_liveupdate_flb_put_incoming();
	}

	return dev->liveupdate.inherit_buses;
}

/**
 * pci_liveupdate_scan_bridge_end() - Finish scanning a PCI bridge
 * @dev: The PCI bridge device.
 * @pass: The scan pass (0 for first pass, 1 for second pass).
 *
 * This function is called by the PCI core when it finishes scanning a bridge.
 * It clears the inheritance status after the second pass so it can be
 * re-evaluated on future scans.
 */
void pci_liveupdate_scan_bridge_end(struct pci_dev *dev, int pass)
{
	/*
	 * Clear inherit_buses after the second pass so it can be re-evaluated
	 * on future scans.
	 */
	if (pass)
		dev->liveupdate.inherit_buses = false;
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

	/*
	 * This device was not preserved across Live Update, or it was preserved
	 * but has already been probed and gone through pci_liveupdate_finish(),
	 * e.g. due to removing and re-adding the device. Either way, it's not
	 * treated as incoming-preserved.
	 */
	if (!dev_ser || !dev_ser->refcount) {
		pci_liveupdate_flb_put_incoming();
		return;
	}


	pci_info(dev, "Device was preserved by previous kernel across Live Update\n");
	dev->liveupdate.incoming = dev_ser;
	dev->liveupdate.was_preserved = true;

	/*
	 * Hold the ref on the incoming FLB until pci_liveupdate_finish() so
	 * that dev->liveupdate.incoming cannot get freed while the PCI core
	 * has a pointer to it. It's better to leak the incoming FLB than do a
	 * use-after-free if driver does not call pci_liveupdate_finish().
	 */
}

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

	if (READ_ONCE(dev->liveupdate.incoming)) {
		pci_WARN(dev, 1, "Destroying incoming-preserved device!\n");
		pci_liveupdate_flb_put_incoming();
	}
}

void pci_liveupdate_freeze(struct pci_dev *dev)
{
	guard(rwsem_write)(&pci_liveupdate.rwsem);
	dev->liveupdate.frozen = 1;
}

static int pci_liveupdate_finish_device(struct pci_ser *ser, struct pci_dev *dev)
{
	if (!dev->liveupdate.incoming) {
		pci_warn(dev, "Cannot finish preserving an unpreserved device\n");
		return -EINVAL;
	}

	if (!dev->liveupdate.incoming->refcount) {
		pci_WARN(dev, 1, "Preserved device has a 0 refcount!\n");
		return -EINVAL;
	}

	/*
	 * Decrement the refcount so this device does not get treated as an
	 * incoming device again, e.g. in case pci_liveupdate_setup_device()
	 * gets called again because the device is hot-plugged.
	 */
	if (--dev->liveupdate.incoming->refcount)
		return 0;

	pci_info(dev, "Device is finished participating in Live Update\n");
	dev->liveupdate.incoming = NULL;
	ser->nr_devices--;
	pci_liveupdate_flb_put_incoming();
	return 0;
}

static void pci_liveupdate_finish_path(struct pci_ser *ser, struct pci_dev *start)
{
	struct pci_dev *dev;

	for_each_pci_dev_in_path(dev, start, NULL) {
		if (pci_liveupdate_finish_device(ser, dev))
			return;
	}
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
 *
 * pci_liveupdate_finish() automatically finishes all bridges upstream of @dev.
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

	pci_liveupdate_finish_path(incoming->ser, dev);
	pci_liveupdate_flb_put_incoming();
}
EXPORT_SYMBOL_GPL(pci_liveupdate_finish);

void pci_liveupdate_init_acs(struct pci_dev *dev)
{
	guard(rwsem_read)(&pci_liveupdate.rwsem);

	if (!dev->acs_cap || !dev->liveupdate.incoming)
		return;

	pci_read_config_word(dev, dev->acs_cap + PCI_ACS_CTRL, &dev->liveupdate.acs_ctrl);
}

int pci_liveupdate_enable_acs(struct pci_dev *dev)
{
	u16 acs_ctrl = dev->liveupdate.acs_ctrl;
	u16 acs_cap = dev->acs_cap;

	/*
	 * Use liveupdate.was_preserved instead of liveupdate.incoming since the
	 * device's ACS controls should not change even after the device is
	 * finished participating in the Live Update.
	 */
	if (!dev->liveupdate.was_preserved)
		return -EINVAL;

	/*
	 * The previous kernel should not have preserved any devices that
	 * require device-specific quirks to enable ACS, but if such a device is
	 * detected (e.g. new device-specific ACS quirk in the current kernel),
	 * log a big warning and fall back to the normal enable ACS path.
	 */
	if (pci_need_dev_specific_enable_acs(dev)) {
		pci_warn(dev, "Device-specific quirk required to enable ACS!\n");
		WARN_ON_ONCE(true);
		return -EINVAL;
	}

	if (acs_cap)
		pci_write_config_word(dev, acs_cap + PCI_ACS_CTRL, acs_ctrl);

	return 0;
}

int pci_liveupdate_configure_ari(struct pci_dev *dev)
{
	u16 val;

	guard(rwsem_read)(&pci_liveupdate.rwsem);

	if (!dev->liveupdate.incoming)
		return -EINVAL;

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL2, &val);
	dev->ari_enabled = !!(val & PCI_EXP_DEVCTL2_ARI);
	return 0;
}

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
