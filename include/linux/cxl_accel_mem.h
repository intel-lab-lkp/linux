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

#define CXL_ACCEL_DRIVER_CAP_HDM	0x1
#define CXL_ACCEL_DRIVER_CAP_MBOX	0x2

typedef struct cxl_dev_state cxl_accel_state;
cxl_accel_state *cxl_accel_state_create(struct device *dev, uint8_t caps);

void cxl_accel_set_dvsec(cxl_accel_state *cxlds, u16 dvsec);
void cxl_accel_set_serial(cxl_accel_state *cxlds, u64 serial);
void cxl_accel_set_resource(struct cxl_dev_state *cxlds, struct resource res,
			    enum accel_resource);
int cxl_pci_accel_setup_regs(struct pci_dev *pdev, struct cxl_dev_state *cxlds);
int cxl_accel_request_resource(struct cxl_dev_state *cxlds, bool is_ram);
void cxl_accel_set_media_ready(struct cxl_dev_state *cxlds);
int cxl_await_media_ready(struct cxl_dev_state *cxlds);

struct cxl_memdev *devm_cxl_add_memdev(struct device *host,
				       struct cxl_dev_state *cxlds);
#endif
