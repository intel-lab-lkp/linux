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
 * Different usecases of i.MX8QM HSIO(High Speed IO) module.
 */
#define	PCIEAX2SATA		1
#define	PCIEAX1PCIEBX1SATA	2
#define	PCIEAX2PCIEBX1		3

#endif /* _DT_BINDINGS_IMX8_PCIE_H */
