/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Advanced Micro Devices, Inc. */

#ifndef __CXL_H
#define __CXL_H

#include <linux/ioport.h>

enum cxl_resource {
	CXL_RES_DPA,
	CXL_RES_RAM,
	CXL_RES_PMEM,
};

struct cxl_dev_state *cxl_accel_state_create(struct device *dev);

void cxl_set_dvsec(struct cxl_dev_state *cxlds, u16 dvsec);
void cxl_set_serial(struct cxl_dev_state *cxlds, u64 serial);
int cxl_set_resource(struct cxl_dev_state *cxlds, struct resource res,
		     enum cxl_resource);
#endif
