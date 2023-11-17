/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHY device list allow maintaining a list of PHY devices that are
 * part of a netdevice's link topology. PHYs can for example be chained,
 * as is the case when using a PHY that exposes an SFP module, on which an
 * SFP transceiver that embeds a PHY is connected.
 *
 * This list can then be used by userspace to leverage individual PHY
 * capabilities.
 */
#ifndef __LINK_TOPOLOGY_H
#define __LINK_TOPOLOGY_H

#include <linux/ethtool.h>
#include <linux/link_topology_core.h>

struct xarray;
struct phy_device;
struct net_device;
struct sfp_bus;

struct phy_device_node {
	enum phy_upstream upstream_type;

	union {
		struct net_device	*netdev;
		struct phy_device	*phydev;
	} upstream;

	struct sfp_bus *parent_sfp_bus;

	struct phy_device *phy;
};

#if IS_ENABLED(CONFIG_PHYLIB)
struct phy_device *link_topo_get_phy(struct link_topology *lt, int phyindex);
int link_topo_add_phy(struct link_topology *lt, struct phy_device *phy,
		      enum phy_upstream upt, void *upstream);

void link_topo_del_phy(struct link_topology *lt, struct phy_device *phy);

#else
static struct phy_device *link_topo_get_phy(struct link_topology *lt, int phyindex)
{
	return NULL;
}

static int link_topo_add_phy(struct link_topology *lt, struct phy_device *phy,
			     enum phy_upstream upt, void *upstream)
{
	return 0;
}

static void link_topo_del_phy(struct link_topology *lt, struct phy_device *phy)
{
}
#endif

#endif /* __LINK_TOPOLOGY_H */
