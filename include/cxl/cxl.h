/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2025 Advanced Micro Devices, Inc. */

#ifndef __CXL_H
#define __CXL_H

#include <linux/types.h>
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

#endif
