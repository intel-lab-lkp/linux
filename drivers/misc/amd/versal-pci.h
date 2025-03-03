/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __VERSAL_PCI_H
#define __VERSAL_PCI_H

#include <linux/configfs.h>
#include <linux/firmware.h>

#define MGMT_BAR		0

#define vdev_info(vdev, fmt, args...)					\
	dev_info(&(vdev)->pdev->dev, "%s: "fmt, __func__, ##args)

#define vdev_warn(vdev, fmt, args...)					\
	dev_warn(&(vdev)->pdev->dev, "%s: "fmt, __func__, ##args)

#define vdev_err(vdev, fmt, args...)					\
	dev_err(&(vdev)->pdev->dev, "%s: "fmt, __func__, ##args)

#define vdev_dbg(vdev, fmt, args...)					\
	dev_dbg(&(vdev)->pdev->dev, fmt, ##args)

struct versal_pci_device;
struct rm_cmd;

struct axlf_header {
	__u64				length;
	__u8				reserved1[24];
	uuid_t				rom_uuid;
	__u8				reserved2[64];
	uuid_t				uuid;
	__u8				reserved3[24];
} __packed;

struct axlf {
	__u8				magic[8];
	__u8				reserved[296];
	struct axlf_header		header;
} __packed;

struct fw_info {
	__u32				opcode;
	char				name[128];
};

struct versal_pci_device {
	struct pci_dev			*pdev;

	struct rm_device		*rdev;
	struct fw_info			fw;

	void __iomem			*io_regs;
	uuid_t				intf_uuid;
	__u8				fw_id[UUID_STRING_LEN + 1];

	struct configfs_subsystem	cfs_subsys;
};

#endif	/* __VERSAL_PCI_H */
