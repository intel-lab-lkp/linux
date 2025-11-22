/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * phy-common-props.h -- Common properties for generic PHYs
 *
 * Copyright 2025 NXP
 */

#ifndef __PHY_COMMON_PROPS_H
#define __PHY_COMMON_PROPS_H

#include <dt-bindings/phy/phy.h>

struct fwnode_handle;

int phy_get_rx_polarity(struct fwnode_handle *fwnode, const char *mode_name,
			unsigned int supported, unsigned int default_val);
int phy_get_tx_polarity(struct fwnode_handle *fwnode, const char *mode_name,
			unsigned int supported, unsigned int default_val);

#endif /* __PHY_COMMON_PROPS_H */
