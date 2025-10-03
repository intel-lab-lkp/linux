// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Author: Evangelos Petrongonas <epetron@amazon.de>
 *
 * Implementation of the PCI Configuration Space Cache (PCSC)
 * PCSC is a module which caches the PCI Configuration Space Accesses
 * It implements a write-invalidate policy, meaning that writes are
 * propagated to the device and invalidating the cache. The registers that
 * we are caching are based on the values that are safe to cache and we
 * are not expecting them to change without OS actions.
 *
 */

 #define pr_fmt(fmt) "PCSC: " fmt

#include <linux/pcsc.h>

static bool pcsc_initialised;

static int pcsc_add_bus(struct pci_bus *bus)
{
	if (!bus->orig_ops || !bus->orig_ops->add_bus)
		return 0;
	return bus->orig_ops->add_bus(bus);
}

static void pcsc_remove_bus(struct pci_bus *bus)
{
	if (bus->orig_ops && bus->orig_ops->remove_bus)
		bus->orig_ops->remove_bus(bus);
}

/**
 * pcsc_map_bus - Map PCI configuration space for memory-mapped access
 * @bus: PCI bus structure
 * @devfn: Device and function number
 * @where: Offset in configuration space
 *
 * WARNING: Cache Bypass Issue
 * This function returns a memory-mapped I/O address that provides direct
 * access to PCI configuration space, completely bypassing the PCSC cache.
 *
 * Any reads or writes performed through the returned MMIO address will NOT:
 * - Use cached values for reads
 * - Update cached values on reads
 * - Invalidate cached values on writes
 *
 * This can lead to cache inconsistency where:
 * 1. PCSC cache contains stale data after MMIO writes
 * 2. Subsequent cached reads return outdated values
 * 3. Cache coherency is lost until the next cache invalidation
 *
 * Current users include:
 * - (pci_generic_config_{read,write}{,32}) which are already handled
 * - operations on RCs that are not supported by PCSC.
 * Therefore, there is no risk of cache inconsistency here.
 * However, any future use of map_bus after cache population poses risks.
 *
 * IMPORTANT: Callers using the returned MMIO address are responsible for
 * maintaining cache consistency. Consider invalidating relevant cache entries
 * after MMIO operations if the device's cache may be active.
 *
 * Return: Virtual address for memory-mapped config space access, or NULL
 */
static void __iomem *pcsc_map_bus(struct pci_bus *bus, unsigned int devfn,
				  int where)
{
	if (!bus->orig_ops || !bus->orig_ops->map_bus)
		return NULL;
	return bus->orig_ops->map_bus(bus, devfn, where);
}

/* Weak references to allow architecture-specific overrides */
int __weak pcsc_hw_config_read(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 *val)
{
	/*
	 * This function is only called from pcsc_cached_config_read,
	 * which means PCSC ops have already been injected and orig_ops
	 * should be valid.
	 */
	if (bus->orig_ops && bus->orig_ops->read)
		return bus->orig_ops->read(bus, devfn, where, size, val);

	*val = 0xffffffff;
	return PCIBIOS_FUNC_NOT_SUPPORTED;
}
EXPORT_SYMBOL_GPL(pcsc_hw_config_read);

int __weak pcsc_hw_config_write(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 val)
{
	/*
	 * This function is only called from pcsc_cached_config_write,
	 * which means PCSC ops have already been injected and orig_ops
	 * should be valid.
	 */
	if (bus->orig_ops && bus->orig_ops->write)
		return bus->orig_ops->write(bus, devfn, where, size, val);

	return PCIBIOS_FUNC_NOT_SUPPORTED;
}
EXPORT_SYMBOL_GPL(pcsc_hw_config_write);

int pcsc_cached_config_read(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, u32 *val)
{
	if (!pcsc_initialised)
		goto read_from_dev;

read_from_dev:
	return pcsc_hw_config_read(bus, devfn, where, size, val);
}
EXPORT_SYMBOL_GPL(pcsc_cached_config_read);

int pcsc_cached_config_write(struct pci_bus *bus, unsigned int devfn, int where,
			     int size, u32 val)
{
	if (!pcsc_initialised)
		goto write_to_dev;

write_to_dev:
	return pcsc_hw_config_write(bus, devfn, where, size, val);
}
EXPORT_SYMBOL_GPL(pcsc_cached_config_write);

static struct pci_ops pcsc_ops = {
	.add_bus = pcsc_add_bus,
	.remove_bus = pcsc_remove_bus,
	.map_bus = pcsc_map_bus,
	.read = pcsc_cached_config_read,
	.write = pcsc_cached_config_write,
};

int pcsc_inject_bus_ops(struct pci_bus *bus)
{
	if (!bus)
		return -EINVAL;

	if (!bus->ops) {
		WARN_ONCE(
			1,
			"PCSC: Cannot inject ops - bus %04x:%02x ops not defined\n",
			pci_domain_nr(bus), bus->number);
		return -EINVAL;
	}

	if (bus->ops->read == pcsc_cached_config_read || bus->orig_ops)
		return 0;

	bus->orig_ops = bus->ops;
	bus->ops = &pcsc_ops;

	pci_dbg(bus, "PCSC: Injected ops for bus");
	return 0;
}
EXPORT_SYMBOL_GPL(pcsc_inject_bus_ops);

static void pcsc_remove_bus_ops(struct pci_bus *bus)
{
	if (bus->orig_ops && bus->ops == &pcsc_ops) {
		bus->ops = bus->orig_ops;
		bus->orig_ops = NULL;
	}
}

static int pcsc_bus_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct device *dev = data;
	struct pci_bus *bus;

	bus = to_pci_bus(dev);
	if (!bus)
		return NOTIFY_OK;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		pcsc_inject_bus_ops(bus);
		break;
	case BUS_NOTIFY_DEL_DEVICE:
		/*
		 * Remove on DEL_DEVICE to unhook before device_del() completes.
		 * This ensures caching is disabled before the final cleanup.
		 */
		pcsc_remove_bus_ops(bus);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block pcsc_bus_nb = {
	.notifier_call = pcsc_bus_notify,
};

static int __init pcsc_init(void)
{
	bus_register_notifier(&pci_bus_type, &pcsc_bus_nb);

	pcsc_initialised = true;
	pr_info("initialised\n");

	return 0;
}

core_initcall(pcsc_init);
