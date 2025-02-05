/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2025 Advanced Micro Devices, Inc. */

#ifndef __CXL_H
#define __CXL_H

#include <linux/types.h>

/* Capabilities as defined for:
 *
 *	Component Registers (Table 8-22 CXL 3.1 specification)
 *	Device Registers (8.2.8.2.1 CXL 3.1 specification)
 *
 * and currently being used for kernel CXL support.
 */

enum cxl_dev_cap {
	/* capabilities from Component Registers */
	CXL_DEV_CAP_RAS,
	CXL_DEV_CAP_HDM,
	/* capabilities from Device Registers */
	CXL_DEV_CAP_DEV_STATUS,
	CXL_DEV_CAP_MAILBOX_PRIMARY,
	CXL_DEV_CAP_MEMDEV,
	CXL_MAX_CAPS,
};

/*
 * enum cxl_devtype - delineate type-2 from a generic type-3 device
 * @CXL_DEVTYPE_DEVMEM - Vendor specific CXL Type-2 device implementing HDM-D or
 *			 HDM-DB, no requirement that this device implements a
 *			 mailbox, or other memory-device-standard manageability
 *			 flows.
 * @CXL_DEVTYPE_CLASSMEM - Common class definition of a CXL Type-3 device with
 *			   HDM-H and class-mandatory memory device registers
 */
enum cxl_devtype {
	CXL_DEVTYPE_DEVMEM,
	CXL_DEVTYPE_CLASSMEM,
};

struct device;
struct cxl_memdev_state *cxl_memdev_state_create(struct device *dev, u64 serial,
					   u16 dvsec, enum cxl_devtype type);
struct pci_dev;
struct cxl_dev_state;
int cxl_pci_accel_setup_regs(struct pci_dev *pdev, struct cxl_memdev_state *cxlmds,
			     unsigned long *caps);
#endif
