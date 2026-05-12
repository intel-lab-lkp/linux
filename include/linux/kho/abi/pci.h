/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2026, Google LLC.
 * David Matlack <dmatlack@google.com>
 */

#ifndef _LINUX_KHO_ABI_PCI_H
#define _LINUX_KHO_ABI_PCI_H

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/types.h>

/**
 * DOC: PCI File-Lifecycle Bound (FLB) Live Update ABI
 *
 * This header defines the ABI for preserving core PCI state across kexec using
 * Live Update File-Lifecycle Bound (FLB) data.
 *
 * This interface is a contract. Any modification to any of the serialization
 * structs defined here constitutes a breaking change. Such changes require
 * incrementing the version number in the PCI_LUO_FLB_COMPATIBLE string.
 */

#define PCI_LUO_FLB_COMPATIBLE "pci-v1"

/**
 * struct pci_dev_ser - Serialized state about a single PCI device.
 *
 * @domain: The device's PCI domain number (segment).
 * @bdf: The device's PCI bus, device, and function number.
 * @padding: Padding to naturally align struct pci_dev_ser.
 */
struct pci_dev_ser {
	u32 domain;
	u16 bdf;
	u16 padding;
} __packed;

/**
 * struct pci_ser - PCI Subsystem Live Update State
 *
 * This struct tracks state about all devices that are being preserved across
 * a Live Update for the next kernel.
 *
 * @max_nr_devices: The length of the devices[] flexible array.
 * @nr_devices: The number of devices that were preserved.
 * @devices: Flexible array of pci_dev_ser structs for each device.
 */
struct pci_ser {
	u32 max_nr_devices;
	u32 nr_devices;
	struct pci_dev_ser devices[];
} __packed;

/* Ensure all elements of devices[] are naturally aligned. */
static_assert(offsetof(struct pci_ser, devices) % sizeof(unsigned long) == 0);
static_assert(sizeof(struct pci_dev_ser) % sizeof(unsigned long) == 0);

#endif /* _LINUX_KHO_ABI_PCI_H */
