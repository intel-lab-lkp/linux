/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PHY_LINK_TOPOLOGY_CORE_H
#define __PHY_LINK_TOPOLOGY_CORE_H

#include <linux/xarray.h>

struct phy_link_topology {
	struct xarray phys;
	u32 next_phy_index;
};

static inline struct phy_link_topology *phy_link_topo_create(struct net_device *dev)
{
	struct phy_link_topology *topo;

	topo = kzalloc(sizeof(*topo), GFP_KERNEL);
	if (!topo)
		return ERR_PTR(-ENOMEM);

	xa_init_flags(&topo->phys, XA_FLAGS_ALLOC1);
	topo->next_phy_index = 1;

	return topo;
}

static inline void phy_link_topo_destroy(struct phy_link_topology *topo)
{
	if (!topo)
		return;

	xa_destroy(&topo->phys);
	kfree(topo);
}

#endif /* __PHY_LINK_TOPOLOGY_CORE_H */
