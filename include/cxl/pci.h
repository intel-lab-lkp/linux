/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2020 Intel Corporation. All rights reserved. */

#ifndef __CXL_ACCEL_PCI_H
#define __CXL_ACCEL_PCI_H

/*
 * See section 8.1 Configuration Space Registers in the CXL 2.0
 * Specification. Names are taken straight from the specification with "CXL" and
 * "DVSEC" redundancies removed. When obvious, abbreviations may be used.
 */
#define PCI_DVSEC_HEADER1_LENGTH_MASK  GENMASK(31, 20)

/* CXL 2.0 8.1.3: PCIe DVSEC for CXL Device */
#define CXL_DVSEC_PCIE_DEVICE					0
#define   CXL_DVSEC_CAP_OFFSET		0xA
#define     CXL_DVSEC_CACHE_CAPABLE	BIT(0)
#define     CXL_DVSEC_MEM_CAPABLE	BIT(2)
#define     CXL_DVSEC_HDM_COUNT_MASK	GENMASK(5, 4)
#define     CXL_DVSEC_CACHE_WBI_CAPABLE	BIT(6)
#define     CXL_DVSEC_CXL_RST_CAPABLE	BIT(7)
#define     CXL_DVSEC_CXL_RST_TIMEOUT_MASK	GENMASK(10, 8)
#define     CXL_DVSEC_CXL_RST_MEM_CLR_CAPABLE	BIT(11)
#define   CXL_DVSEC_CTRL_OFFSET		0xC
#define     CXL_DVSEC_MEM_ENABLE	BIT(2)
#define   CXL_DVSEC_CTRL2_OFFSET	0x10
#define     CXL_DVSEC_DISABLE_CACHING	BIT(0)
#define     CXL_DVSEC_INIT_CACHE_WBI	BIT(1)
#define     CXL_DVSEC_INIT_CXL_RESET	BIT(2)
#define     CXL_DVSEC_CXL_RST_MEM_CLR_ENABLE	BIT(3)
#define   CXL_DVSEC_STATUS2_OFFSET	0x12
#define     CXL_DVSEC_CACHE_INVALID	BIT(0)
#define     CXL_DVSEC_CXL_RST_COMPLETE	BIT(1)
#define     CXL_DVSEC_CXL_RESET_ERR	BIT(2)
#define   CXL_DVSEC_RANGE_SIZE_HIGH(i)	(0x18 + ((i) * 0x10))
#define   CXL_DVSEC_RANGE_SIZE_LOW(i)	(0x1C + ((i) * 0x10))
#define     CXL_DVSEC_MEM_INFO_VALID	BIT(0)
#define     CXL_DVSEC_MEM_ACTIVE	BIT(1)
#define     CXL_DVSEC_MEM_SIZE_LOW_MASK	GENMASK(31, 28)
#define   CXL_DVSEC_RANGE_BASE_HIGH(i)	(0x20 + ((i) * 0x10))
#define   CXL_DVSEC_RANGE_BASE_LOW(i)	(0x24 + ((i) * 0x10))
#define     CXL_DVSEC_MEM_BASE_LOW_MASK	GENMASK(31, 28)

#define CXL_DVSEC_RANGE_MAX		2

/* CXL 2.0 8.1.4: Non-CXL Function Map DVSEC */
#define CXL_DVSEC_FUNCTION_MAP					2

/* CXL 2.0 8.1.5: CXL 2.0 Extensions DVSEC for Ports */
#define CXL_DVSEC_PORT_EXTENSIONS				3
#define   CXL_DVSEC_PORT_CTL		0xC
#define     CXL_DVSEC_UNMASK_SBR		BIT(0)

/* CXL 2.0 8.1.6: GPF DVSEC for CXL Port */
#define CXL_DVSEC_PORT_GPF					4
#define   CXL_DVSEC_PORT_GPF_PHASE_1_CONTROL_OFFSET		0x0C
#define     CXL_DVSEC_PORT_GPF_PHASE_1_TMO_BASE_MASK		GENMASK(3, 0)
#define     CXL_DVSEC_PORT_GPF_PHASE_1_TMO_SCALE_MASK		GENMASK(11, 8)
#define   CXL_DVSEC_PORT_GPF_PHASE_2_CONTROL_OFFSET		0xE
#define     CXL_DVSEC_PORT_GPF_PHASE_2_TMO_BASE_MASK		GENMASK(3, 0)
#define     CXL_DVSEC_PORT_GPF_PHASE_2_TMO_SCALE_MASK		GENMASK(11, 8)

/* CXL 2.0 8.1.7: GPF DVSEC for CXL Device */
#define CXL_DVSEC_DEVICE_GPF					5

/* CXL 2.0 8.1.8: PCIe DVSEC for Flex Bus Port */
#define CXL_DVSEC_PCIE_FLEXBUS_PORT				7

/* CXL 2.0 8.1.9: Register Locator DVSEC */
#define CXL_DVSEC_REG_LOCATOR					8
#define   CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET			0xC
#define     CXL_DVSEC_REG_LOCATOR_BIR_MASK			GENMASK(2, 0)
#define	    CXL_DVSEC_REG_LOCATOR_BLOCK_ID_MASK			GENMASK(15, 8)
#define     CXL_DVSEC_REG_LOCATOR_BLOCK_OFF_LOW_MASK		GENMASK(31, 16)

#endif
