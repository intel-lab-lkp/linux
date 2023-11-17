// SPDX-License-Identifier: GPL-2.0+
/*
 * Infrastructure to handle all PHY devices connected to a given netdev,
 * either directly or indirectly attached.
 *
 * Copyright (c) 2023 Maxime Chevallier<maxime.chevallier@bootlin.com>
 */

#include <linux/link_topology.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/xarray.h>

struct phy_device *link_topo_get_phy(struct link_topology *lt, int phyindex)
{
	struct phy_device_node *pdn = xa_load(&lt->phys, phyindex);

	if (pdn)
		return pdn->phy;

	return NULL;
}
EXPORT_SYMBOL_GPL(link_topo_get_phy);

int link_topo_add_phy(struct link_topology *lt, struct phy_device *phy,
		      enum phy_upstream upt, void *upstream)
{
	struct phy_device_node *pdn;
	int ret;

	pdn = kzalloc(sizeof(*pdn), GFP_KERNEL);
	if (!pdn)
		return -ENOMEM;

	pdn->phy = phy;
	switch (upt) {
	case PHY_UPSTREAM_MAC:
		pdn->upstream.netdev = (struct net_device *)upstream;
		if (phy_on_sfp(phy))
			pdn->parent_sfp_bus = pdn->upstream.netdev->sfp_bus;
		break;
	case PHY_UPSTREAM_PHY:
		pdn->upstream.phydev = (struct phy_device *)upstream;
		if (phy_on_sfp(phy))
			pdn->parent_sfp_bus = pdn->upstream.phydev->sfp_bus;
		break;
	default:
		ret = -EINVAL;
		goto err;
	}
	pdn->upstream_type = upt;

	ret = xa_alloc_cyclic(&lt->phys, &phy->phyindex, pdn, xa_limit_32b,
			      &lt->next_phy_index, GFP_KERNEL);
	if (ret)
		goto err;

	return 0;

err:
	kfree(pdn);
	return ret;
}
EXPORT_SYMBOL_GPL(link_topo_add_phy);

void link_topo_del_phy(struct link_topology *lt, struct phy_device *phy)
{
	struct phy_device_node *pdn = xa_erase(&lt->phys, phy->phyindex);

	kfree(pdn);
}
EXPORT_SYMBOL_GPL(link_topo_del_phy);

void link_topo_init(struct link_topology *lt)
{
	xa_init_flags(&lt->phys, XA_FLAGS_ALLOC1);
	lt->next_phy_index = 1;
}
