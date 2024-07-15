/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Advanced Micro Devices, Inc. */

#ifndef __CXL_ACCEL_PCI_H
#define __CXL_ACCEL_PCI_H

/* CXL 2.0 8.1.3: PCIe DVSEC for CXL Device */
#define CXL_DVSEC_PCIE_DEVICE					0
#define   CXL_DVSEC_CAP_OFFSET		0xA
#define     CXL_DVSEC_MEM_CAPABLE	BIT(2)
#define     CXL_DVSEC_HDM_COUNT_MASK	GENMASK(5, 4)
#define   CXL_DVSEC_CTRL_OFFSET		0xC
#define     CXL_DVSEC_MEM_ENABLE	BIT(2)
#define   CXL_DVSEC_RANGE_SIZE_HIGH(i)	(0x18 + (i * 0x10))
#define   CXL_DVSEC_RANGE_SIZE_LOW(i)	(0x1C + (i * 0x10))
#define     CXL_DVSEC_MEM_INFO_VALID	BIT(0)
#define     CXL_DVSEC_MEM_ACTIVE	BIT(1)
#define     CXL_DVSEC_MEM_SIZE_LOW_MASK	GENMASK(31, 28)
#define   CXL_DVSEC_RANGE_BASE_HIGH(i)	(0x20 + (i * 0x10))
#define   CXL_DVSEC_RANGE_BASE_LOW(i)	(0x24 + (i * 0x10))
#define     CXL_DVSEC_MEM_BASE_LOW_MASK	GENMASK(31, 28)

#endif
