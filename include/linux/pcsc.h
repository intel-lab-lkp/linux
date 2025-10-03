/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2025 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Author: Evangelos Petrongonas <epetron@amazon.de>
 *
 */

#ifndef _LINUX_PCSC_H
#define _LINUX_PCSC_H

#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/bitmap.h>

#ifdef CONFIG_PCIE_PCSC
#define PCSC_CFG_SPC_SIZE (4 * SZ_1K)
#else
#define PCSC_CFG_SPC_SIZE 256
#endif

struct pcsc_node {
	u8 *cfg_space;
	DECLARE_BITMAP(cachable_bitmask, PCSC_CFG_SPC_SIZE);
	DECLARE_BITMAP(cached_bitmask, PCSC_CFG_SPC_SIZE);
};

/**
 * pcsc_hw_config_read - Direct hardware PCI config space read
 * @bus: PCI bus
 * @devfn: PCI device function
 * @where: offset in PCI config space
 * @size: size of data to read
 * @val: pointer to store read data
 *
 * This function performs a direct hardware read from PCI configuration space,
 * bypassing the PCSC cache. It is a weak function that can be overridden by
 * architecture-specific implementations.
 *
 * Return: 0 on success, non-zero error code on failure
 */
int pcsc_hw_config_read(struct pci_bus *bus, unsigned int devfn, int where,
			int size, u32 *val);

/**
 * pcsc_hw_config_write - Direct hardware PCI config space write
 * @bus: PCI bus
 * @devfn: PCI device function
 * @where: offset in PCI config space
 * @size: size of data to write
 * @val: value to write
 *
 * This function performs a direct hardware write to PCI configuration space,
 * bypassing the PCSC cache. It is a weak function that can be overridden by
 * architecture-specific implementations.
 *
 * Return: 0 on success, non-zero error code on failure
 */
int pcsc_hw_config_write(struct pci_bus *bus, unsigned int devfn, int where,
			 int size, u32 val);

/**
 * pcsc_cached_config_read - Read PCI config space register via PCSC
 * @bus: PCI bus
 * @devfn: PCI device function
 * @where: offset in PCI config space
 * @size: size of data to read
 * @val: pointer to store read data
 *
 * Reads a register from the PCI configuration space of a device using the
 * PCSC infrastructure.
 *
 * Return: 0 on success, non-zero error code on failure
 */
int pcsc_cached_config_read(struct pci_bus *bus, unsigned int devfn, int where,
			    int size, u32 *val);

/**
 * pcsc_cached_config_write - Write PCI config space register via PCSC
 * @bus: PCI bus
 * @devfn: PCI device function
 * @where: offset in PCI config space
 * @size: size of data to write
 * @val: value to write
 *
 * Writes a value to a register in the PCI configuration space of a device using
 * the PCSC infrastructure.
 *
 * Return: 0 on success, non-zero error code on failure
 */
int pcsc_cached_config_write(struct pci_bus *bus, unsigned int devfn, int where,
			     int size, u32 val);

/**
 * pcsc_inject_bus_ops Inject the pcsc ops into bus pci_ops
 * @bus: the bus in which to inject the ops
 *
 * Return: 0 on success, negative error code on failure
 */
int pcsc_inject_bus_ops(struct pci_bus *bus);

/**
 * pcsc_add_device - Allocate and initialize a new PCSC node
 * This should only be called once for each device
 * @dev: PCI device to initialise the cache for
 *
 * Returns: 0 on success error code on failure
 */
int pcsc_add_device(struct pci_dev *dev);

/**
 * pcsc_remove_device - Clear up any PCSC data
 * @dev: PCI device to remove
 *
 * Returns: 0 on success, -EINVAL if dev is NULL
 */
int pcsc_remove_device(struct pci_dev *dev);

/**
 * @brief Returns if the PCSC infrastructure is initialised
 *
 */
bool pcsc_is_initialised(void);

#endif /* _LINUX_PCSC_H */
