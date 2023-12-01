/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PHY_LINK_TOPOLOGY_CORE_H
#define __PHY_LINK_TOPOLOGY_CORE_H

struct xarray;

struct phy_link_topology {
	struct xarray phys;

	u32 next_phy_index;
};

void phy_link_topo_init(struct phy_link_topology *topo);

#endif /* __PHY_LINK_TOPOLOGY_CORE_H */
