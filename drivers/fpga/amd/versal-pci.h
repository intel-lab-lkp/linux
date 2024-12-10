/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __VERSAL_PCI_H
#define __VERSAL_PCI_H

#include <linux/firmware.h>
#include <linux/fpga/fpga-mgr.h>

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
struct comm_chan_device;

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

struct fw_tnx {
	struct rm_cmd			*cmd;
	__u32				opcode;
	__u32				id;
};

struct fpga_device {
	enum fpga_mgr_states		state;
	struct fpga_manager		*mgr;
	struct versal_pci_device	*vdev;
	struct fw_tnx			fw;
};

struct firmware_device {
	struct versal_pci_device	*vdev;
	struct fw_upload		*fw;
	__u8				*name;
	__u32				fw_name_id;
	struct rm_cmd			*cmd;
	__u32				id;
	uuid_t				uuid;
};

struct versal_pci_device {
	struct pci_dev			*pdev;

	struct fpga_device		*fdev;
	struct comm_chan_device         *ccdev;
	struct firmware_device		*fwdev;
	struct device			*device;

	void __iomem			*io_regs;
	uuid_t				xclbin_uuid;
	uuid_t				intf_uuid;
	__u8				fw_id[UUID_STRING_LEN + 1];

	__u8				*debugfs_root;
};

/* versal pci driver APIs */
int versal_pci_load_xclbin(struct versal_pci_device *vdev, uuid_t *xclbin_uuid);

#endif	/* __VERSAL_PCI_H */
