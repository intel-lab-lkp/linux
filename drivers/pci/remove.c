// SPDX-License-Identifier: GPL-2.0
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include "pci.h"

static void pci_stop_bus(struct pci_bus *bus);
static void pci_remove_bus_device(struct pci_dev *dev);

static void pci_free_resources(struct pci_dev *dev)
{
	struct resource *res;

	pci_dev_for_each_resource(dev, res) {
		if (res->parent)
			release_resource(res);
	}
}

static void pci_stop_dev(struct pci_dev *dev)
{
	if (!pci_dev_test_and_clear_added(dev))
		return;

	pci_pme_active(dev, false);
	of_platform_depopulate(&dev->dev);
	device_release_driver(&dev->dev);
	pci_proc_detach_device(dev);
	pci_remove_sysfs_dev_files(dev);
	of_pci_remove_node(dev);
}

static void pci_destroy_dev(struct pci_dev *dev)
{
	if (pci_dev_test_and_set_removed(dev))
		return;

	device_del(&dev->dev);

	down_write(&pci_bus_sem);
	list_del(&dev->bus_list);
	up_write(&pci_bus_sem);

	pci_doe_destroy(dev);
	pcie_aspm_exit_link_state(dev);
	pci_bridge_d3_update(dev);
	pci_free_resources(dev);
	put_device(&dev->dev);
}

static void pci_clear_bus(struct pci_bus *bus)
{
	struct pci_dev *dev, *next;

	pci_lock_bus(bus);
	list_for_each_entry_safe(dev, next, &bus->devices, bus_list)
		pci_remove_bus_device(dev);
	pci_unlock_bus(bus);
}

void pci_remove_bus(struct pci_bus *bus)
{
	pci_clear_bus(bus);
	pci_proc_detach_bus(bus);

	down_write(&pci_bus_sem);
	list_del(&bus->node);
	pci_bus_release_busn_res(bus);
	up_write(&pci_bus_sem);
	pci_remove_legacy_files(bus);

	if (bus->ops->remove_bus)
		bus->ops->remove_bus(bus);

	pcibios_remove_bus(bus);
	device_unregister(&bus->dev);
}
EXPORT_SYMBOL(pci_remove_bus);

static void pci_stop_bus_device(struct pci_dev *dev)
{
	struct pci_bus *bus = pci_dev_get_subordinate(dev);

	if (bus) {
		pci_stop_bus(bus);
		pci_bus_put(bus);
	}
	pci_stop_dev(dev);
}

static void pci_stop_bus(struct pci_bus *bus)
{
	struct pci_dev *dev, *next;

	/*
	 * Stopping an SR-IOV PF device removes all the associated VFs,
	 * which will update the bus->devices list and confuse the
	 * iterator.  Therefore, iterate in reverse so we remove the VFs
	 * first, then the PF.
	 */
	pci_lock_bus(bus);
	list_for_each_entry_safe_reverse(dev, next, &bus->devices, bus_list)
		pci_stop_bus_device(dev);
	pci_unlock_bus(bus);
}

static void pci_remove_bus_device(struct pci_dev *dev)
{
	struct pci_bus *bus;

	down_write(&pci_bus_sem);
	bus = pci_dev_get_subordinate(dev);
	if (bus) {
		WRITE_ONCE(dev->subordinate, NULL);
		up_write(&pci_bus_sem);

		pci_remove_bus(bus);
		pci_bus_put(bus);
	} else
		up_write(&pci_bus_sem);
	pci_destroy_dev(dev);
}

/**
 * pci_stop_and_remove_bus_device - remove a PCI device and any children
 * @dev: the device to remove
 *
 * Remove a PCI device from the device lists, informing the drivers
 * that the device has been removed.  We also remove any subordinate
 * buses and children in a depth-first manner.
 *
 * For each device we remove, delete the device structure from the
 * device lists, remove the /proc entry, and notify userspace
 * (/sbin/hotplug).
 */
void pci_stop_and_remove_bus_device(struct pci_dev *dev)
{
	pci_stop_bus_device(dev);
	pci_remove_bus_device(dev);
}
EXPORT_SYMBOL(pci_stop_and_remove_bus_device);

void pci_stop_and_remove_bus_device_locked(struct pci_dev *dev)
{
	struct pci_bus *bus = pci_bus_get(dev->bus);

	pci_lock_rescan_remove();
	pci_lock_bus(bus);
	pci_stop_and_remove_bus_device(dev);
	pci_unlock_bus(bus);
	pci_unlock_rescan_remove();

	pci_bus_put(bus);
}
EXPORT_SYMBOL_GPL(pci_stop_and_remove_bus_device_locked);

void pci_stop_root_bus(struct pci_bus *bus)
{
	struct pci_host_bridge *host_bridge;

	if (!pci_is_root_bus(bus))
		return;

	host_bridge = to_pci_host_bridge(bus->bridge);
	pci_stop_bus(bus);

	/* stop the host bridge */
	device_release_driver(&host_bridge->dev);
}
EXPORT_SYMBOL_GPL(pci_stop_root_bus);

void pci_remove_root_bus(struct pci_bus *bus)
{
	struct pci_host_bridge *host_bridge;

	if (!pci_is_root_bus(bus))
		return;

	host_bridge = to_pci_host_bridge(bus->bridge);

#ifdef CONFIG_PCI_DOMAINS_GENERIC
	/* Release domain_nr if it was dynamically allocated */
	if (host_bridge->domain_nr == PCI_DOMAIN_NR_NOT_SET)
		pci_bus_release_domain_nr(bus, host_bridge->dev.parent);
#endif

	pci_remove_bus(bus);
	host_bridge->bus = NULL;

	/* remove the host bridge */
	device_del(&host_bridge->dev);
}
EXPORT_SYMBOL_GPL(pci_remove_root_bus);
