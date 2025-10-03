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

#include <linux/atomic.h>
#include <linux/pcsc.h>

static bool pcsc_initialised;
static atomic_t num_nodes = ATOMIC_INIT(0);

inline bool pcsc_is_initialised(void)
{
	return pcsc_initialised;
}

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

static inline int _test_bits(int where, int size, const void *addr)
{
	int i;
	int res = 1;

	for (i = 0; i < size; i++)
		res &= test_bit(where + i, addr);
	return res;
}

static int pcsc_is_access_cacheable(struct pci_dev *dev, int where, int size)
{
	if (unlikely(!dev || (where + size > PCSC_CFG_SPC_SIZE)))
		return 0;

	return _test_bits(where, size, dev->pcsc->cachable_bitmask);
}

static inline bool pcsc_is_cached(struct pci_dev *dev, int where, int size)
{
	if (unlikely(!dev || !dev->pcsc || !dev->pcsc->cfg_space ||
		     (where + size > PCSC_CFG_SPC_SIZE)))
		return 0;

	return _test_bits(where, size, dev->pcsc->cached_bitmask);
}

static inline void pcsc_set_cached(struct pci_dev *dev, int where, bool cached)
{
	if (WARN_ON(!dev))
		return;

	if (WARN_ON(where >= PCSC_CFG_SPC_SIZE))
		return;

	if (cached)
		set_bit(where, dev->pcsc->cached_bitmask);
	else
		clear_bit(where, dev->pcsc->cached_bitmask);
}

static int pcsc_get_byte(struct pci_dev *dev, int where, u8 *val)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return -EINVAL;

	if (WARN_ON(where >= PCSC_CFG_SPC_SIZE))
		return -EPERM;
	*val = dev->pcsc->cfg_space[where];
	return 0;
}

static int pcsc_update_byte(struct pci_dev *dev, int where, u8 val)
{
	if (WARN_ON(!dev || !dev->pcsc || !dev->pcsc->cfg_space))
		return -EINVAL;

	if (WARN_ON(where >= PCSC_CFG_SPC_SIZE))
		return -EPERM;
	dev->pcsc->cfg_space[where] = val;
	pcsc_set_cached(dev, where, true);

	return 0;
}

int pcsc_add_device(struct pci_dev *dev)
{
	struct pcsc_node *node;
	struct pci_bus *bus;

	if (WARN_ON(!dev))
		return -EINVAL;

	bus = dev->bus;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	dev->pcsc = node;
	/* The current version of the PCSC supports only endpoint devices.
	 * Bridges and RCs are not supported, but we are still creating
	 * nodes for these devices, as it simplifies the code flow
	 */
	if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL) {
		dev->pcsc->cfg_space = kzalloc(PCSC_CFG_SPC_SIZE, GFP_KERNEL);
		if (!dev->pcsc->cfg_space)
			goto err_free_node;

	} else {
		dev->pcsc->cfg_space = NULL;
	}

	atomic_inc(&num_nodes);
	pci_dbg(dev, "PCSC: Created cache node\n");

	return 0;

err_free_node:
	dev->pcsc = NULL;
	kfree(node);
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(pcsc_add_device);

int pcsc_remove_device(struct pci_dev *dev)
{
	if (WARN_ON(!dev))
		return -EINVAL;

	pci_dbg(dev, "PCSC: Removing cache node");

	atomic_dec(&num_nodes);

	if (dev->pcsc && dev->pcsc->cfg_space) {
		kfree(dev->pcsc->cfg_space);
		kfree(dev->pcsc);
	}
	dev->pcsc = NULL;

	return 0;
}
EXPORT_SYMBOL_GPL(pcsc_remove_device);

/**
 * pcsc_get_and_insert_multiple - Read multiple bytes from PCI cache or HW
 * @dev: PCI device to read from
 * @bus: PCI bus to read from
 * @devfn: device and function number
 * @where: offset in config space
 * @word: pointer to store read value
 * @size: number of bytes to read (1, 2 or 4)
 *
 * Reads consecutive bytes from PCI cache or hardware. If values are not cached,
 * reads from hardware and inserts into cache.
 *
 * Return: 0 on success, negative error code on failure
 */
static int pcsc_get_and_insert_multiple(struct pci_dev *dev,
					struct pci_bus *bus, unsigned int devfn,
					int where, u32 *word, int size)
{
	u32 word_cached = 0;
	u8 byte_val;
	int rc, i;

	if (WARN_ON(!dev || !bus || !word))
		return -EINVAL;

	if (WARN_ON(size != 1 && size != 2 && size != 4))
		return -EINVAL;

	/* Check bounds */
	if (where + size > PCSC_CFG_SPC_SIZE)
		return -EINVAL;

	if (pcsc_is_cached(dev, where, size)) {
		/* Read bytes from cache and assemble them into word_cached
		 * in little-endian order (as per PCI spec)
		 */
		for (i = 0; i < size; i++) {
			pcsc_get_byte(dev, where + i, &byte_val);
			word_cached |= ((u32)byte_val << (i * 8));
		}
	} else {
		rc = pcsc_hw_config_read(bus, devfn, where, size, &word_cached);
		if (rc) {
			pci_err(dev,
				"%s: Failed to read CFG Space where=%d size=%d",
				__func__, where, size);
			return rc;
		}

		/* Extract bytes from word_cached in little-endian order
		 * and store them in cache.
		 */
		for (i = 0; i < size; i++) {
			byte_val = (word_cached >> (i * 8)) & 0xFF;
			pcsc_update_byte(dev, where + i, byte_val);
		}
	}

	*word = word_cached;
	return 0;
}

int pcsc_cached_config_read(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, u32 *val)
{
	int rc;
	struct pci_dev *dev;

	if (unlikely(!pcsc_is_initialised()))
		goto read_from_dev;

	if (WARN_ON(!bus || !val || (size != 1 && size != 2 && size != 4) ||
		    where + size > PCSC_CFG_SPC_SIZE))
		return -EINVAL;

	dev = pci_get_slot(bus, devfn);

	if (unlikely(!dev || !dev->pcsc))
		goto read_from_dev;

	if (dev->pcsc->cfg_space &&
	    pcsc_is_access_cacheable(dev, where, size)) {
		rc = pcsc_get_and_insert_multiple(dev, bus, devfn, where, val,
						  size);
		if (likely(!rc)) {
			pci_dev_put(dev);
			return 0;
		}
		/* if reading from the cache failed continue and try reading
		 * from the actual device
		 */
	}
read_from_dev:
	if (dev)
		pci_dev_put(dev);
	return pcsc_hw_config_read(bus, devfn, where, size, val);
}
EXPORT_SYMBOL_GPL(pcsc_cached_config_read);

int pcsc_cached_config_write(struct pci_bus *bus, unsigned int devfn, int where,
			     int size, u32 val)
{
	int i;
	struct pci_dev *dev;

	if (unlikely(!pcsc_is_initialised()))
		goto write_to_dev;

	if (WARN_ON(!bus || (size != 1 && size != 2 && size != 4) ||
		    where + size > PCSC_CFG_SPC_SIZE))
		return -EINVAL;

	dev = pci_get_slot(bus, devfn);

	if (unlikely(!dev || !dev->pcsc || !dev->pcsc->cfg_space)) {
		/* Do not add nodes on arbitrary writes  */
		goto write_to_dev;
	} else {
		/* Mark the cache as dirty */
		if (pcsc_is_access_cacheable(dev, where, size)) {
			for (i = 0; i < size; i++)
				pcsc_set_cached(dev, where + i, false);
		}
	}
write_to_dev:
	if (dev)
		pci_dev_put(dev);
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
