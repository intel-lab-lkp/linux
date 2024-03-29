/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * This header provides constants for i.MX8 PCIe.
 */

#ifndef _DT_BINDINGS_IMX8_PCIE_H
#define _DT_BINDINGS_IMX8_PCIE_H

/* Reference clock PAD mode */
#define IMX8_PCIE_REFCLK_PAD_UNUSED	0
#define IMX8_PCIE_REFCLK_PAD_INPUT	1
#define IMX8_PCIE_REFCLK_PAD_OUTPUT	2

/*
 * i.MX8QM HSIO subsystem has three lane PHYs and three controllers:
 * PCIEA(2 lanes capapble PCIe controller), PCIEB (only support one
 * lane) and SATA.
 * In the different use cases. PCIEA can be binded to PHY lane0, lane1
 * or Lane0 and lane1. PCIEB can be binded to lane1 or lane2 PHY. SATA
 * can only be binded to last lane2 PHY.
 * Define i.MX8Q HSIO controller ID here to specify the controller
 * binded to the PHY.
 * Meanwhile, i.MX8QXP HSIO subsystem has one lane PHY and PCIEB(only
 * support one lane) controller.
 */
#define IMX8Q_HSIO_PCIEA_ID	0
#define IMX8Q_HSIO_PCIEB_ID	1
#define IMX8Q_HSIO_SATA_ID	2

/*
 * On i.MX8QM, PCIEA is mandatory required if the HSIO is enabled.
 * Define configurations beside PCIEA is enabled.
 * On i.MX8QXP, HSIO module only has PCIEB and one lane PHY.
 * The "IMX8Q_HSIO_CFG_PCIEB" can be used on i.MX8QXP platforms.
 */
#define IMX8Q_HSIO_CFG_SATA		1
#define IMX8Q_HSIO_CFG_PCIEB		2
#define IMX8Q_HSIO_CFG_PCIEBSATA	3

#endif /* _DT_BINDINGS_IMX8_PCIE_H */
