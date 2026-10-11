/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Red Hat */

#ifndef _QCOM_VFIO_PCI_H_
#define _QCOM_VFIO_PCI_H_

#include <linux/vfio_pci_core.h>
#include <linux/qcom_vfio.h>

/**
 * struct qcom_vfio_pci_device - Qualcomm VFIO PCI device with quirks
 * @vdev: Core VFIO PCI device
 * @msi_offset: Base offset in extended config for MSI passthrough (0 if disabled)
 * @msi_cached: Whether host MSI address/data has been cached
 * @msi_addr_lo: Cached host MSI address (low 32 bits)
 * @msi_addr_hi: Cached host MSI address (high 32 bits)
 * @msi_data: Cached host MSI data value
 */
struct qcom_vfio_pci_device {
	struct vfio_pci_core_device vdev;

	/* MSI quirk state */
	int msi_offset;
	bool msi_cached;
	u32 msi_addr_lo;
	u32 msi_addr_hi;
	u16 msi_data;
};

/* MSI quirk functions */
ssize_t qcom_vfio_pci_config_rw(struct qcom_vfio_pci_device *qdev,
				char __user *buf, size_t count,
				loff_t offset, bool iswrite);
void qcom_cache_and_write_msi(struct qcom_vfio_pci_device *qdev);

#endif /* _QCOM_VFIO_PCI_H_ */
