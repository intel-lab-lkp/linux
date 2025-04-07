/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2024 Sean Anderson <sean.anderson@seco.com>
 */

#ifndef PCS_XILINX_H
#define PCS_XILINX_H

#include <linux/err.h>

struct device;
struct phylink_pcs;

#ifdef CONFIG_PCS_XILINX
/**
 * axienet_xilinx_pcs_get() - Compatibility function for the AXI Ethernet driver
 * @dev: The MAC device
 * @interfaces: The interfaces to use as a fallback
 *
 * This is a helper function for the AXI Ethernet driver to ensure backwards
 * compatibility with device trees which do not include compatible strings for
 * the PCS. It should not be used by new code.
 *
 * Return: a PCS, or an error pointer
 */
struct phylink_pcs *axienet_xilinx_pcs_get(struct device *dev,
					   const unsigned long *interfaces);
#else
static inline struct phylink_pcs *
axienet_xilinx_pcs_get(struct device *dev, const unsigned long *interfaces)
{
	return ERR_PTR(-ENODEV);
}
#endif

#endif /* PCS_XILINX_H */
