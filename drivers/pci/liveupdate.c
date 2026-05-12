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
 * bound to driver B in the incoming kernel.
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
 * while there are preserved devices, itss secondary and subordinate bus numbers
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

#define pr_fmt(fmt) "PCI: liveupdate: " fmt

#include <linux/io.h>
#include <linux/kexec_handover.h>
#include <linux/kho/abi/pci.h>
#include <linux/liveupdate.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/pci.h>

#include "liveupdate.h"

/*
 * During a Live Update, preserved devices are allowed to continue performing
 * memory transactions. The kernel must not change the fabric topology,
 * including bus numbers, since that would require disabling and flushing any
 * memory transactions first.
 *
 * To keep things simple, inherit the secondary and subordinate bus numbers on
 * _all_ bridges if _any_ PCI devices are preserved (i.e.  even bridges without
 * any downstream endpoints that were preserved).  This avoids accidentally
 * assigning a bridge a new window that overlaps with a preserved device that is
 * downstream of a different bridge.
 */
static atomic_t inherit_buses;

/**
 * struct pci_flb_outgoing - Outgoing PCI FLB object
 * @ser: The outgoing struct pci_ser for the next kernel.
 * @lock: Lock used to protect against changes to @ser.
 */
struct pci_flb_outgoing {
	struct pci_ser *ser;
	struct mutex lock;
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
	return domain << 16 | bdf;
}

bool pci_liveupdate_inherit_buses(void)
{
	return atomic_read(&inherit_buses);
}

static void pci_set_liveupdate_inherit_buses(bool enable)
{
	/* Ensure updates to inherit_buses do not race with rescans */
	pci_lock_rescan_remove();

	/*
	 * Increment/decrement instead of setting directly to true/false so that
	 * pci_liveupdate_inherit_buses() returns true if any device is outgoing
	 * preserved or incoming preserved.
	 */
	if (enable)
		atomic_inc(&inherit_buses);
	else
		atomic_dec(&inherit_buses);

	pci_unlock_rescan_remove();
}

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

	pci_set_liveupdate_inherit_buses(true);
	return 0;
}

static void pci_flb_unpreserve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_outgoing *outgoing = args->obj;

	pci_set_liveupdate_inherit_buses(false);

	WARN_ON_ONCE(outgoing->ser->nr_devices);
	kho_unpreserve_free(outgoing->ser);
	kfree(outgoing);

	pr_debug("Unpreserved struct pci_ser\n");
}

static int pci_flb_retrieve(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_incoming *incoming;
	int i, ret;

	incoming = kmalloc_obj(*incoming);
	if (!incoming)
		return -ENOMEM;

	incoming->ser = phys_to_virt(args->data);

	xa_init(&incoming->xa);

	for (i = 0; i < incoming->ser->max_nr_devices; i++) {
		struct pci_dev_ser *dev_ser = &incoming->ser->devices[i];
		unsigned long key;

		if (!dev_ser->refcount)
			continue;

		key = pci_ser_xa_key(dev_ser->domain, dev_ser->bdf);
		ret = xa_err(xa_store(&incoming->xa, key, dev_ser, GFP_KERNEL));
		if (ret) {
			xa_destroy(&incoming->xa);
			kfree(incoming);
			return ret;
		}
	}

	args->obj = incoming;

	pci_set_liveupdate_inherit_buses(true);
	return 0;
}

static void pci_flb_finish(struct liveupdate_flb_op_args *args)
{
	struct pci_flb_incoming *incoming = args->obj;

	pci_set_liveupdate_inherit_buses(false);

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

static int pci_liveupdate_unpreserve_device(struct pci_ser *ser, struct pci_dev *dev)
{
	struct pci_dev_ser *dev_ser;

	guard(write_lock)(&dev->liveupdate.lock);

	if (dev->liveupdate.frozen) {
		pci_WARN(dev, 1, "Cannot unpreserve device after it is frozen!\n");
		return -EINVAL;
	}

	dev_ser = dev->liveupdate.outgoing;
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
	ser->nr_devices--;
	memset(dev_ser, 0, sizeof(*dev_ser));
	dev->liveupdate.outgoing = NULL;
	return 0;
}

static int pci_liveupdate_preserve_device_existing(struct pci_dev *dev)
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

static int pci_liveupdate_preserve_device_new(struct pci_ser *ser, struct pci_dev *dev)
{
	int i;

	/*
	 * Do not preserve a devices that rely on device-specific ACS
	 * equivalents (for now) since that would complicate keeping ACS
	 * flags constant across Live Update.
	 */
	if (dev->dev_flags & PCI_DEV_FLAGS_ACS_ENABLED_QUIRK) {
		pci_warn(dev, "Refusing to preserve device that relies on ACS quirks\n");
		return -EINVAL;
	}

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

static int pci_liveupdate_preserve_device(struct pci_ser *ser, struct pci_dev *dev)
{
	guard(write_lock)(&dev->liveupdate.lock);

	if (dev->liveupdate.frozen) {
		pci_WARN(dev, 1, "Cannot preserve device after it is frozen!\n");
		return -EINVAL;
	}

	if (dev->liveupdate.outgoing)
		return pci_liveupdate_preserve_device_existing(dev);
	else
		return pci_liveupdate_preserve_device_new(ser, dev);
}

static int pci_liveupdate_preserve_path(struct pci_ser *ser, struct pci_dev *dev)
{
	int ret;

	if (!dev)
		return 0;

	ret = pci_liveupdate_preserve_device(ser, dev);
	if (ret)
		return ret;

	ret = pci_liveupdate_preserve_path(ser, dev->bus->self);
	if (ret) {
		pci_liveupdate_unpreserve_device(ser, dev);
		return ret;
	}

	return 0;
}

/**
 * pci_liveupdate_preserve() - Preserve a PCI device across Live Update
 * @dev: The PCI device to preserve.
 *
 * pci_liveupdate_preserve() notifies the PCI core that a PCI device should be
 * preserved across the next Live Update. Drivers must call
 * pci_liveupdate_preserve() from their struct liveupdate_file_handler
 * preserve() callback to ensure the outgoing struct pci_ser is allocated.
 *
 * pci_liveupdate_preserve() automatically preserves all bridges upstream of
 * @dev.
 *
 * Returns: 0 on success, <0 on failure.
 */
int pci_liveupdate_preserve(struct pci_dev *dev)
{
	struct pci_flb_outgoing *outgoing = NULL;
	int ret;

	if (dev->is_virtfn)
		return -EINVAL;

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&outgoing);
	if (ret)
		return ret;

	if (!outgoing)
		return -ENOENT;

	guard(mutex)(&outgoing->lock);
	return pci_liveupdate_preserve_path(outgoing->ser, dev);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_preserve);

static void pci_liveupdate_unpreserve_path(struct pci_ser *ser, struct pci_dev *dev)
{
	if (!dev)
		return;

	if (pci_liveupdate_unpreserve_device(ser, dev))
		return;

	pci_liveupdate_unpreserve_path(ser, dev->bus->self);
}

/**
 * pci_liveupdate_unpreserve() - Cancel preservation of a PCI device
 * @dev: The PCI device to preserve.
 *
 * pci_liveupdate_unpreserve() notifies the PCI core that a PCI device should no
 * longer be preserved across the next Live Update. Drivers must call
 * pci_liveupdate_unpreserve() from their struct liveupdate_file_handler
 * unpreserve() callback to ensure the outgoing struct pci_ser is allocated.
 *
 * pci_liveupdate_unpreserve() automatically unpreserves all bridges upstream of
 * @dev.
 */
void pci_liveupdate_unpreserve(struct pci_dev *dev)
{
	struct pci_flb_outgoing *outgoing = NULL;
	int ret;

	ret = liveupdate_flb_get_outgoing(&pci_liveupdate_flb, (void **)&outgoing);

	if (ret || !outgoing) {
		pci_warn(dev, "Cannot unpreserve device without outgoing Live Update state\n");
		return;
	}

	guard(mutex)(&outgoing->lock);
	pci_liveupdate_unpreserve_path(outgoing->ser, dev);
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
	guard(write_lock)(&dev->liveupdate.lock);
	dev->liveupdate.incoming = dev_ser;
	dev->liveupdate.was_preserved = true;

	/*
	 * Hold the ref on the incoming FLB until pci_liveupdate_finish() so
	 * that dev->liveupdate.incoming does not get freed while it is in use.
	 */
}

void pci_liveupdate_cleanup_device(struct pci_dev *dev)
{
	bool incoming;

	scoped_guard(write_lock, &dev->liveupdate.lock)
		incoming = !!xchg(&dev->liveupdate.incoming, NULL);

	/*
	 * Drop the FLB reference acquired in pci_liveupdate_setup_device() if
	 * the device is being cleaned up before pci_liveupdate_finish(), e.g.
	 * due to allocation failure during setup.
	 *
	 * Do not drop dev->liveupdate.incoming->refcount since this device has
	 * not gone through pci_liveupdate_finish() and thus is still an
	 * incoming preserved device.
	 */
	if (incoming)
		pci_liveupdate_flb_put_incoming();
}

void pci_liveupdate_freeze(struct pci_dev *dev)
{
	guard(write_lock)(&dev->liveupdate.lock);
	dev->liveupdate.frozen = 1;
}

static int __pci_liveupdate_finish_device(struct pci_dev *dev)
{
	guard(write_lock)(&dev->liveupdate.lock);

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
		return -EBUSY;

	pci_info(dev, "Device is finished participating in Live Update\n");
	dev->liveupdate.incoming = NULL;
	return 0;
}

static int pci_liveupdate_finish_device(struct pci_dev *dev)
{
	int ret;

	/*
	 * If ret == -EBUSY the device is still preserved due to remaining
	 * references. Return 0 up to the caller to indicate it should proceed
	 * to finish preserving upstream devices but do not drop the device's
	 * reference on the incoming FLB below.
	 */
	ret = __pci_liveupdate_finish_device(dev);
	if (ret)
		return ret == -EBUSY ? 0 : ret;

	/*
	 * Once the device's refcount reaches zero drop the device's reference
	 * on the incoming FLB so it can be freed.
	 */
	pci_liveupdate_flb_put_incoming();
	return 0;
}

/**
 * pci_liveupdate_finish() - Finish the preservation of a PCI device across Live Update
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
	if (!dev)
		return;

	if (pci_liveupdate_finish_device(dev))
		return;

	pci_liveupdate_finish(dev->bus->self);
}
EXPORT_SYMBOL_GPL(pci_liveupdate_finish);

void pci_liveupdate_init_acs(struct pci_dev *dev)
{
	guard(read_lock)(&dev->liveupdate.lock);

	if (!dev->acs_cap || !dev->liveupdate.incoming)
		return;

	pci_read_config_word(dev, dev->acs_cap + PCI_ACS_CTRL, &dev->liveupdate.acs_ctrl);
}

bool pci_liveupdate_inherit_acs(struct pci_dev *dev)
{
	guard(read_lock)(&dev->liveupdate.lock);

	/*
	 * Use liveupdate.was_preserved instead of liveupdate.incoming since the
	 * device's ACS controls should not change even after the device is
	 * finished participating in the Live Update.
	 */
	if (!dev->acs_cap || !dev->liveupdate.was_preserved)
		return false;

	pci_write_config_word(dev, dev->acs_cap + PCI_ACS_CTRL, dev->liveupdate.acs_ctrl);
	return true;
}

bool pci_liveupdate_inherit_ari(struct pci_dev *dev)
{
	u16 val;

	guard(read_lock)(&dev->liveupdate.lock);

	if (!dev->liveupdate.incoming)
		return false;

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL2, &val);
	dev->ari_enabled = !!(val & PCI_EXP_DEVCTL2_ARI);
	return true;
}

/**
 * pci_liveupdate_is_incoming() - Check if a device is incoming preserved
 * @dev: The PCI device to check
 *
 * Check if a device was preserved across Live Update by the previous kernel,
 * i.e. the device is incoming preserved. Note that a device is only considered
 * incoming preserved prior to pci_liveupdate_finish(). It is up to drivers to
 * synchronize usage of pci_liveupdate_is_incoming() with their own call to
 * pci_liveupdate_finish() to avoid acting on stale data.
 *
 * Returns: True if the device is incoming preserved, false otherwise.
 */
bool pci_liveupdate_is_incoming(struct pci_dev *dev)
{
	guard(read_lock)(&dev->liveupdate.lock);
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
