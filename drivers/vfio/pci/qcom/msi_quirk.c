// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright Red Hat

#include <linux/kernel.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/vfio_pci_core.h>

#include "qcom_vfio_pci.h"

/**
 * qcom_cache_and_write_msi - Cache host MSI address and data
 * @qdev: Qualcomm VFIO device
 *
 * Called after QEMU allocates MSI via SET_IRQS ioctl to cache the physical
 * host MSI addresses that will be returned to the VM driver.
 */
void qcom_cache_and_write_msi(struct qcom_vfio_pci_device *qdev)
{
	struct pci_dev *pdev = qdev->vdev.pdev;
	struct device *dev = &pdev->dev;
	struct msi_desc *desc;
	u32 addr_lo, addr_hi;
	u16 data;

	if (qdev->msi_cached)
		return;

	if (!qdev->msi_offset)
		return;

	__msi_lock_descs(dev);
	desc = msi_first_desc(dev, MSI_DESC_ASSOCIATED);
	if (!desc) {
		__msi_unlock_descs(dev);
		pci_warn(pdev, "MSI descriptor not found\n");
		return;
	}

	addr_lo = desc->msg.address_lo;
	addr_hi = desc->msg.address_hi;
	data = desc->msg.data;
	__msi_unlock_descs(dev);

	pci_dbg(pdev, "MSI descriptor - addr_lo=0x%x addr_hi=0x%x data=0x%x\n",
		 addr_lo, addr_hi, data);

	/*
	 * Validate: address must be valid. Data can be 0 with interrupt remapping.
	 * When Intel VT-d interrupt remapping is enabled, the MSI data field
	 * contains an index into the interrupt remapping table, which can be 0.
	 * We cache it anyway since that's what the VM driver will program.
	 */
	if (!addr_lo) {
		pci_warn(pdev, "MSI address invalid, skipping cache\n");
		return;
	}
	qdev->msi_addr_lo = addr_lo;
	qdev->msi_addr_hi = addr_hi;
	qdev->msi_data = data;
	qdev->msi_cached = true;

	pci_write_config_dword(pdev, qdev->msi_offset + QCOM_VFIO_MSI_MAGIC_OFFSET,
			       QCOM_VFIO_MSI_MAGIC);
	pci_write_config_dword(pdev, qdev->msi_offset + QCOM_VFIO_MSI_ADDR_LO_OFFSET,
			       qdev->msi_addr_lo);
	pci_write_config_dword(pdev, qdev->msi_offset + QCOM_VFIO_MSI_ADDR_HI_OFFSET,
			       qdev->msi_addr_hi);
	pci_write_config_word(pdev, qdev->msi_offset + QCOM_VFIO_MSI_DATA_OFFSET,
			      qdev->msi_data);

	pci_info(pdev, "Qualcomm WiFi MSI cached at offset 0x%x - addr_lo=0x%x addr_hi=0x%x data=0x%x (magic=QMSI)\n",
		 qdev->msi_offset, qdev->msi_addr_lo, qdev->msi_addr_hi, qdev->msi_data);
}

/**
 * qcom_intercept_msi_read - Intercept MSI config reads and return cached values
 * @qdev: Qualcomm VFIO device
 * @pos: Config space offset
 * @buf: User buffer
 * @count: Read size
 *
 * Returns: Number of bytes read, or negative error
 */
static ssize_t qcom_intercept_msi_read(struct qcom_vfio_pci_device *qdev,
				       loff_t pos, char __user *buf, size_t count)
{
	u32 tmp_val = 0;
	int msi_magic, msi_addr_lo, msi_addr_hi, msi_data;

	if (!qdev->msi_offset)
		return -ENOTTY;

	msi_magic = qdev->msi_offset + QCOM_VFIO_MSI_MAGIC_OFFSET;
	msi_addr_lo = qdev->msi_offset + QCOM_VFIO_MSI_ADDR_LO_OFFSET;
	msi_addr_hi = qdev->msi_offset + QCOM_VFIO_MSI_ADDR_HI_OFFSET;
	msi_data = qdev->msi_offset + QCOM_VFIO_MSI_DATA_OFFSET;

	if (pos == msi_magic && count == sizeof(u32)) {
		if (qdev->msi_cached) {
			tmp_val = QCOM_VFIO_MSI_MAGIC;
			pci_dbg(qdev->vdev.pdev, "Intercepting MAGIC read at 0x%llx, returning 0x%x (QMSI)\n",
				 pos, tmp_val);
		} else {
			return -ENOTTY;
		}
	} else if (qdev->msi_cached) {
		if (pos == msi_addr_lo && count == sizeof(u32)) {
			tmp_val = qdev->msi_addr_lo;
			pci_dbg(qdev->vdev.pdev, "Intercepting ADDR_LO read at 0x%llx, returning 0x%x\n",
				 pos, tmp_val);
		} else if (pos == msi_addr_hi && count == sizeof(u32)) {
			tmp_val = qdev->msi_addr_hi;
			pci_dbg(qdev->vdev.pdev, "Intercepting ADDR_HI read at 0x%llx, returning 0x%x\n",
				 pos, tmp_val);
		} else if (pos == msi_data && count == sizeof(u16)) {
			tmp_val = qdev->msi_data;
			pci_dbg(qdev->vdev.pdev, "Intercepting MSI_DATA read at 0x%llx, returning 0x%x\n",
				 pos, tmp_val);
		} else {
			return -ENOTTY;
		}
	} else {
		return -ENOTTY;
	}

	if (copy_to_user(buf, &tmp_val, count))
		return -EFAULT;

	return count;
}

/**
 * qcom_vfio_pci_config_rw - Config space read/write with MSI interception
 * @qdev: Qualcomm VFIO device
 * @buf: User buffer
 * @count: Size
 * @offset: Config space offset (0-0xFFF)
 * @iswrite: true for write, false for read
 *
 * Returns: Number of bytes transferred, or -ENOTTY if not intercepted
 */
ssize_t qcom_vfio_pci_config_rw(struct qcom_vfio_pci_device *qdev,
				char __user *buf, size_t count,
				loff_t offset, bool iswrite)
{
	int msi_range_start, msi_range_end;

	if (iswrite)
		return -ENOTTY;

	if (!qdev->msi_offset)
		return -ENOTTY;

	msi_range_start = qdev->msi_offset;
	msi_range_end = qdev->msi_offset + QCOM_VFIO_MSI_SPACE_SIZE;

	if (offset >= msi_range_start && offset < msi_range_end)
		return qcom_intercept_msi_read(qdev, offset, buf, count);

	return -ENOTTY;
}
