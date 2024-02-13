/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2023, Intel Corporation. */

#ifndef _ICE_SF_ETH_H_
#define _ICE_SF_ETH_H_

#include "ice.h"
#include "devlink/ice_devlink_port.h"

int
ice_sf_eth_activate(struct ice_dynamic_port *dyn_port,
		    struct netlink_ext_ack *extack);
void ice_sf_eth_deactivate(struct ice_dynamic_port *dyn_port);

#endif /* _ICE_SF_ETH_H_ */
