/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2015-2016 Freescale Semiconductor, Inc.
 * Copyright 2016-2023, 2025 NXP
 */

#ifndef PCIE_S32G_REGS_H
#define PCIE_S32G_REGS_H

/* PCIe controller Sub-System */

/* Link Interrupt Control And Status */
#define PCIE_S32G_LINK_INT_CTRL_STS		0x40
#define LINK_REQ_RST_NOT_INT_EN			BIT(1)
#define LINK_REQ_RST_NOT_CLR			BIT(2)

/* PCIe controller 0 General Control 1 */
#define PCIE_S32G_PE0_GEN_CTRL_1		0x50
#define DEVICE_TYPE_MASK			GENMASK(3, 0)
#define DEVICE_TYPE(x)				FIELD_PREP(DEVICE_TYPE_MASK, x)
#define SRIS_MODE				BIT(8)

/* PCIe controller 0 General Control 3 */
#define PCIE_S32G_PE0_GEN_CTRL_3		0x58
#define LTSSM_EN				BIT(0)

/* PCIe Controller 0 Transmit Message Request */
#define PCIE_S32G_PE0_TX_MSG_REQ		0x80
#define PME_TURN_OFF_REQ			BIT(19)

/* PCIe Controller 0 Link Debug 2 */
#define PCIE_S32G_PE0_LINK_DBG_2		0xB4
#define SMLH_LTSSM_STATE_MASK			GENMASK(5, 0)
#define SMLH_LINK_UP				BIT(6)
#define RDLH_LINK_UP				BIT(7)

#endif  /* PCI_S32G_REGS_H */
