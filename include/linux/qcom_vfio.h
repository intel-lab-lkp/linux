/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Red Hat */

#ifndef _QCOM_VFIO_H_
#define _QCOM_VFIO_H_

#include <linux/pci.h>

/*
 * MSI Address Passthrough Quirk
 *
 * Qualcomm device firmware requires physical host MSI addresses rather than
 * guest IOVA addresses. The qcom-vfio-pci variant driver caches host MSI
 * values in extended PCI config space with a magic signature for VM
 * discovery.
 *
 * Protocol:
 *  1. Host: qcom-vfio-pci finds free space in extended config (0x100-0xFFF)
 *  2. Host: Writes magic signature "QMSI" (0x49534D51) followed by MSI data
 *  3. VM: Device driver searches for "QMSI" signature to locate cached MSI
 *  4. VM: Reads MSI values and programs device firmware
 *
 * This avoids offset mismatch when QEMU filters certain capabilities, causing
 * host and VM to see different extended capability chains.
 *
 * Layout in extended config space:
 *   +0x00: Magic signature "QMSI" (0x49534D51)
 *   +0x04: MSI address low 32 bits
 *   +0x08: MSI address high 32 bits
 *   +0x0C: MSI data (16 bits)
 *
 * Applicable to:
 *  - ath11k WiFi (PCI ID 0x17cb:0x1103)
 *  - ath12k WiFi (PCI ID 0x17cb:0x1107)
 */

/* Magic signature "QMSI" to identify cached MSI data */
#define QCOM_VFIO_MSI_MAGIC		0x49534D51

/* Space needed for magic + MSI passthrough data */
#define QCOM_VFIO_MSI_SPACE_SIZE	16

/* Relative offsets from base discovered by qcom_vfio_find_msi_offset() */
#define QCOM_VFIO_MSI_MAGIC_OFFSET	0x00	/* Magic signature "QMSI" */
#define QCOM_VFIO_MSI_ADDR_LO_OFFSET	0x04	/* Host MSI address low 32 bits */
#define QCOM_VFIO_MSI_ADDR_HI_OFFSET	0x08	/* Host MSI address high 32 bits */
#define QCOM_VFIO_MSI_DATA_OFFSET	0x0C	/* Host MSI data (16 bits) */

/**
 * qcom_vfio_find_msi_offset - Find free space for host to cache MSI
 * @pdev: PCI device
 *
 * Host-side function: Finds free space in extended config to write MSI cache.
 * Uses PCI core utility to scan capability chain.
 *
 * Returns: Offset in extended config space (>= 0x100), or 0 if no space found
 */
static inline int qcom_vfio_find_msi_offset(struct pci_dev *pdev)
{
	return pci_find_free_ext_cap_offset(pdev, QCOM_VFIO_MSI_SPACE_SIZE);
}

/**
 * qcom_vfio_find_msi_cache - Locate cached MSI data by magic signature
 * @pdev: PCI device
 *
 * VM-side function: Searches extended config space for "QMSI" magic signature
 * to locate host-cached MSI values. This avoids offset mismatch when QEMU
 * filters certain capabilities.
 *
 * Returns: Offset of MSI data (after magic), or 0 if not found
 */
static inline int qcom_vfio_find_msi_cache(struct pci_dev *pdev)
{
	u32 magic;
	int offset;

	for (offset = 0x100; offset <= 0xf00; offset += 4) {
		if (pci_read_config_dword(pdev, offset, &magic))
			continue;
		if (magic == QCOM_VFIO_MSI_MAGIC)
			return offset;
	}

	return 0;
}

#endif /* _QCOM_VFIO_H_ */
