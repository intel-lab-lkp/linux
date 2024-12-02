/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Advanced Micro Devices, Inc. */

#ifndef __CXL_H
#define __CXL_H

#include <linux/ioport.h>
#include <linux/pci.h>

enum cxl_resource {
	CXL_RES_DPA,
	CXL_RES_RAM,
	CXL_RES_PMEM,
};

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
	CXL_MAX_CAPS = 64
};

struct cxl_dev_state *cxl_accel_state_create(struct device *dev);

void cxl_set_dvsec(struct cxl_dev_state *cxlds, u16 dvsec);
void cxl_set_serial(struct cxl_dev_state *cxlds, u64 serial);
int cxl_set_resource(struct cxl_dev_state *cxlds, struct resource res,
		     enum cxl_resource);
bool cxl_pci_check_caps(struct cxl_dev_state *cxlds,
			unsigned long *expected_caps,
			unsigned long *current_caps);
int cxl_pci_accel_setup_regs(struct pci_dev *pdev, struct cxl_dev_state *cxlds);
int cxl_request_resource(struct cxl_dev_state *cxlds, enum cxl_resource type);
int cxl_release_resource(struct cxl_dev_state *cxlds, enum cxl_resource type);
#endif
