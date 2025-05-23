/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/* Copyright 2020 NXP
 * Lynx PCS helpers
 */

#ifndef __LINUX_PCS_LYNX_H
#define __LINUX_PCS_LYNX_H

struct device;
struct mii_bus;
struct phylink_pcs;

struct phylink_pcs *lynx_pcs_create_mdiodev(struct device *dev,
					    struct mii_bus *bus, int addr);
struct phylink_pcs *lynx_pcs_create_fwnode(struct device *dev,
					   struct fwnode_handle *node);

#endif /* __LINUX_PCS_LYNX_H */
