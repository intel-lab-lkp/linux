/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PHY device namespaces allow maintaining a list of PHY devices that are
 * part of a netdevice's link topology. PHYs can for example be chained,
 * as is the case when using a PHY that exposes an SFP module, on which an
 * SFP transceiver that embeds a PHY is connected.
 *
 * This list can then be used by userspace to leverage individual PHY
 * capabilities.
 */
#ifndef __PHY_NS_H
#define __PHY_NS_H

struct mutex;

struct phy_namespace {
	struct list_head phys;
	int last_attributed_index;

	/* Protects the .phys list */
	struct mutex ns_lock;
};

struct phy_device *phy_ns_get_by_index(struct phy_namespace *phy_ns,
				       int phyindex);
void phy_ns_add_phy(struct phy_namespace *phy_ns, struct phy_device *phy);
void phy_ns_del_phy(struct phy_namespace *phy_ns, struct phy_device *phy);
void phy_ns_init(struct phy_namespace *phy_ns);

#endif /* __PHY_NS_H */
