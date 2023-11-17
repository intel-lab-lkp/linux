/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINK_TOPOLOGY_CORE_H
#define __LINK_TOPOLOGY_CORE_H

struct xarray;

struct link_topology {
	struct xarray phys;
	struct xarray ports;

	u32 next_phy_index;
	u32 next_port_index;
};

void link_topo_init(struct link_topology *lt);

#endif /* __LINK_TOPOLOGY_CORE_H */
