/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 * Copyright 2016-2023, 2025 NXP
 */

#ifndef PCIE_S32G_REGS_H
#define PCIE_S32G_REGS_H

/* Instance PCIE_SS - CTRL register offsets (ctrl base) */
#define LINK_INT_CTRL_STS			0x40
#define LINK_REQ_RST_NOT_INT_EN			BIT(1)
#define LINK_REQ_RST_NOT_CLR			BIT(2)

/* PCIe controller 0 general control 1 (ctrl base) */
#define PE0_GEN_CTRL_1				0x50
#define SS_DEVICE_TYPE_MASK			GENMASK(3, 0)
#define SS_DEVICE_TYPE(x)			FIELD_PREP(SS_DEVICE_TYPE_MASK, x)
#define SRIS_MODE_EN				BIT(8)

/* PCIe controller 0 general control 3 (ctrl base) */
#define PE0_GEN_CTRL_3				0x58
/* LTSSM Enable. Active high. Set it low to hold the LTSSM in Detect state. */
#define LTSSM_EN				BIT(0)

/* PCIe Controller 0 Link Debug 2 (ctrl base) */
#define PCIE_SS_PE0_LINK_DBG_2			0xB4
#define PCIE_SS_SMLH_LTSSM_STATE_MASK		GENMASK(5, 0)
#define PCIE_SS_SMLH_LINK_UP			BIT(6)
#define PCIE_SS_RDLH_LINK_UP			BIT(7)
#define LTSSM_STATE_L0				0x11U /* L0 state */
#define LTSSM_STATE_L0S				0x12U /* L0S state */
#define LTSSM_STATE_L1_IDLE			0x14U /* L1_IDLE state */
#define LTSSM_STATE_HOT_RESET			0x1FU /* HOT_RESET state */

/* PCIe Controller 0  Interrupt Status (ctrl base) */
#define PE0_INT_STS				0xE8
#define HP_INT_STS				BIT(6)

/* Link Control and Status Register. (PCI_EXP_LNKCTL in pci-regs.h) */
#define PCIE_CAP_LINK_TRAINING			BIT(27)

/* Instance PCIE_PORT_LOGIC - DBI register offsets */
#define PCIE_PORT_LOGIC_BASE			0x700

/* ACE Cache Coherency Control Register 3 */
#define PORT_LOGIC_COHERENCY_CONTROL_1		(PCIE_PORT_LOGIC_BASE + 0x1E0)
#define PORT_LOGIC_COHERENCY_CONTROL_2		(PCIE_PORT_LOGIC_BASE + 0x1E4)
#define PORT_LOGIC_COHERENCY_CONTROL_3		(PCIE_PORT_LOGIC_BASE + 0x1E8)

/*
 * See definition of register "ACE Cache Coherency Control Register 1"
 * (COHERENCY_CONTROL_1_OFF) in the SoC RM
 */
#define CC_1_MEMTYPE_BOUNDARY_MASK		GENMASK(31, 2)
#define CC_1_MEMTYPE_BOUNDARY(x)		FIELD_PREP(CC_1_MEMTYPE_BOUNDARY_MASK, x)
#define CC_1_MEMTYPE_VALUE			BIT(0)
#define CC_1_MEMTYPE_LOWER_PERIPH		0x0
#define CC_1_MEMTYPE_LOWER_MEM			0x1

#endif  /* PCI_S32G_REGS_H */
