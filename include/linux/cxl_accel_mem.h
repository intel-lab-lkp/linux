/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Advanced Micro Devices, Inc. */

#include <linux/cdev.h>

#ifndef __CXL_ACCEL_MEM_H
#define __CXL_ACCEL_MEM_H

enum accel_resource{
	CXL_ACCEL_RES_DPA,
	CXL_ACCEL_RES_RAM,
	CXL_ACCEL_RES_PMEM,
};

typedef struct cxl_dev_state cxl_accel_state;
cxl_accel_state *cxl_accel_state_create(struct device *dev);

void cxl_accel_set_dvsec(cxl_accel_state *cxlds, u16 dvsec);
void cxl_accel_set_serial(cxl_accel_state *cxlds, u64 serial);
void cxl_accel_set_resource(struct cxl_dev_state *cxlds, struct resource res,
			    enum accel_resource);
int cxl_pci_accel_setup_regs(struct pci_dev *pdev, struct cxl_dev_state *cxlds);
#endif
