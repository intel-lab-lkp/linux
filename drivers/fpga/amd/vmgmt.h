/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __VMGMT_H
#define __VMGMT_H

#include <linux/cdev.h>
#include <linux/dev_printk.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/firmware.h>
#include <linux/fpga/fpga-bridge.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/fpga/fpga-region.h>
#include <linux/pci.h>
#include <linux/uuid.h>
#include <linux/regmap.h>

#define AMD_VMGMT_BAR			0
#define AMD_VMGMT_BAR_MASK		BIT(0)

#define vmgmt_info(vdev, fmt, args...)					\
	dev_info(&(vdev)->pdev->dev, "%s: "fmt, __func__, ##args)

#define vmgmt_warn(vdev, fmt, args...)					\
	dev_warn(&(vdev)->pdev->dev, "%s: "fmt, __func__, ##args)

#define vmgmt_err(vdev, fmt, args...)					\
	dev_err(&(vdev)->pdev->dev, "%s: "fmt, __func__, ##args)

#define vmgmt_dbg(vdev, fmt, args...)					\
	dev_dbg(&(vdev)->pdev->dev, fmt, ##args)

struct vmgmt_device;
struct comms_device;
struct rm_cmd;

struct axlf_header {
	u64				length;
	unsigned char			reserved1[24];
	uuid_t				rom_uuid;
	unsigned char			reserved2[64];
	uuid_t				uuid;
	unsigned char			reserved3[24];
} __packed;

struct axlf {
	char				magic[8];
	unsigned char			reserved[296];
	struct axlf_header		header;
} __packed;

struct fw_tnx {
	struct rm_cmd		*cmd;
	int			opcode;
	int			id;
};

struct fpga_device {
	enum fpga_mgr_states	state;
	struct fpga_manager	*mgr;
	struct fpga_bridge	*bridge;
	struct fpga_region	*region;
	struct vmgmt_device	*vdev;
	struct fw_tnx		fw;
};

struct firmware_device {
	struct vmgmt_device	*vdev;
	struct fw_upload	*fw;
	char			*name;
	u32			fw_name_id;
	struct rm_cmd		*cmd;
	int			id;
	uuid_t			uuid;
};

struct vmgmt_device {
	struct pci_dev		*pdev;

	struct rm_device	*rdev;
	struct comms_device	*ccdev;
	struct fpga_device	*fdev;
	struct firmware_device	*fwdev;
	struct cdev		cdev;
	struct device		*device;

	int                     minor;
	void __iomem		*tbl;
	uuid_t			xclbin_uuid;
	uuid_t			intf_uuid;

	void                    *debugfs_root;
};

#endif	/* __VMGMT_H */
