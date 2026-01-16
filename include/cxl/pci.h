/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2020 Intel Corporation. All rights reserved. */

#ifndef __CXL_ACCEL_PCI_H
#define __CXL_ACCEL_PCI_H

/* HDM Decoder state for save/restore */
struct cxl_hdm_decoder_state {
	u64 base;
	u64 size;
	u32 ctrl;
	u64 dpa_skip;
	bool enabled;
};

#define CXL_MAX_DECODERS 10

/* CXL Type 2 device state for save/restore across reset */
struct cxl_type2_saved_state {
	/* DVSEC registers */
	u16 dvsec_ctrl;
	u16 dvsec_ctrl2;

	/* HDM Decoder registers */
	u32 hdm_decoder_count;
	u32 hdm_global_ctrl;
	struct cxl_hdm_decoder_state decoders[CXL_MAX_DECODERS];

	/* IDE registers */
	u32 ide_cap;
	u32 ide_ctrl;
	u32 ide_key_refresh_time;
	u32 ide_truncation_delay;
};

int cxl_config_save_state(struct pci_dev *pdev,
			  struct cxl_type2_saved_state *state);
int cxl_config_restore_state(struct pci_dev *pdev,
			     const struct cxl_type2_saved_state *state);

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
#define     CXL_DVSEC_CTRL_RWL_MASK	0x5FED
#define   CXL_DVSEC_CTRL2_OFFSET	0x10
#define     CXL_DVSEC_DISABLE_CACHING	BIT(0)
#define     CXL_DVSEC_INIT_CACHE_WBI	BIT(1)
#define     CXL_DVSEC_INIT_CXL_RESET	BIT(2)
#define     CXL_DVSEC_CXL_RST_MEM_CLR_ENABLE	BIT(3)
#define   CXL_DVSEC_STATUS2_OFFSET	0x12
#define     CXL_DVSEC_CACHE_INVALID	BIT(0)
#define     CXL_DVSEC_CXL_RST_COMPLETE	BIT(1)
#define     CXL_DVSEC_CXL_RESET_ERR	BIT(2)
#define   CXL_DVSEC_LOCK_OFFSET		0x14
#define     CXL_DVSEC_LOCK_CONFIG_LOCK	BIT(0)
#define   CXL_DVSEC_RANGE_SIZE_HIGH(i)	(0x18 + ((i) * 0x10))
#define   CXL_DVSEC_RANGE_SIZE_LOW(i)	(0x1C + ((i) * 0x10))
#define     CXL_DVSEC_MEM_INFO_VALID	BIT(0)
#define     CXL_DVSEC_MEM_ACTIVE	BIT(1)
#define     CXL_DVSEC_MEM_SIZE_LOW_MASK	GENMASK(31, 28)
#define   CXL_DVSEC_RANGE_BASE_HIGH(i)	(0x20 + ((i) * 0x10))
#define   CXL_DVSEC_RANGE_BASE_LOW(i)	(0x24 + ((i) * 0x10))
#define     CXL_DVSEC_MEM_BASE_LOW_MASK	GENMASK(31, 28)

#define CXL_DVSEC_RANGE_MAX		2

/* CXL HDM Decoder Capability Structure (Section 8.2.4.20) */
#define CXL_HDM_DECODER_CAP_OFFSET		0x0
#define   CXL_HDM_DECODER_COUNT_MASK		GENMASK(3, 0)
#define CXL_HDM_DECODER_GLOBAL_CTRL_OFFSET	0x4
#define   CXL_HDM_DECODER_ENABLE		BIT(1)
/* CXL HDM Decoder n registers (Offset 20h*n + base) */
#define CXL_HDM_DECODER_BASE_LOW(n)		(0x10 + ((n) * 0x20))
#define CXL_HDM_DECODER_BASE_HIGH(n)		(0x14 + ((n) * 0x20))
#define CXL_HDM_DECODER_SIZE_LOW(n)		(0x18 + ((n) * 0x20))
#define CXL_HDM_DECODER_SIZE_HIGH(n)		(0x1C + ((n) * 0x20))
#define CXL_HDM_DECODER_CTRL(n)			(0x20 + ((n) * 0x20))
#define CXL_HDM_DECODER_DPA_SKIP_LOW(n)		(0x24 + ((n) * 0x20))
#define CXL_HDM_DECODER_DPA_SKIP_HIGH(n)	(0x28 + ((n) * 0x20))

/* CXL IDE Capability Structure (Section 8.2.4.22) */
#define CXL_IDE_CAP_OFFSET			0x00
#define   CXL_IDE_CAP_CAPABLE			BIT(0)
#define CXL_IDE_CTRL_OFFSET			0x04
#define CXL_IDE_KEY_REFRESH_TIME_CTRL_OFFSET	0x18
#define CXL_IDE_TRUNCATION_DELAY_CTRL_OFFSET	0x1C

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
