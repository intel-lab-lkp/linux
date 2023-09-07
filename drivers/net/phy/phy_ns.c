// SPDX-License-Identifier: GPL-2.0+
/*
 * Infrastructure to handle all PHY devices connected to a given netdev,
 * either directly or indirectly attached.
 *
 * Copyright (c) 2023 Maxime Chevallier<maxime.chevallier@bootlin.com>
 */

#include <linux/phy.h>
#include <linux/phy_ns.h>

static int phy_ns_next_phyindex(struct phy_namespace *phy_ns)
{
	int phyindex = phy_ns->last_attributed_index;

	for (;;) {
		if (++phyindex <= 0)
			phyindex = 1;
		if (!phy_ns_get_by_index(phy_ns, phyindex))
			return phy_ns->last_attributed_index = phyindex;
	}
}

struct phy_device *phy_ns_get_by_index(struct phy_namespace *phy_ns,
				       int phyindex)
{
	struct phy_device *phy;

	mutex_lock(&phy_ns->ns_lock);
	list_for_each_entry(phy, &phy_ns->phys, node)
		if (phy->phyindex == phyindex)
			goto unlock;

	phy = NULL;
unlock:
	mutex_unlock(&phy_ns->ns_lock);
	return phy;
}
EXPORT_SYMBOL_GPL(phy_ns_get_by_index);

void phy_ns_add_phy(struct phy_namespace *phy_ns, struct phy_device *phy)
{
	/* PHYs can be attached and detached, they will keep their id */
	if (!phy->phyindex)
		phy->phyindex = phy_ns_next_phyindex(phy_ns);

	mutex_lock(&phy_ns->ns_lock);
	list_add(&phy->node, &phy_ns->phys);
	mutex_unlock(&phy_ns->ns_lock);
}
EXPORT_SYMBOL_GPL(phy_ns_add_phy);

void phy_ns_del_phy(struct phy_namespace *phy_ns, struct phy_device *phy)
{
	mutex_lock(&phy_ns->ns_lock);
	list_del(&phy->node);
	mutex_unlock(&phy_ns->ns_lock);
}
EXPORT_SYMBOL_GPL(phy_ns_del_phy);

void phy_ns_init(struct phy_namespace *phy_ns)
{
	INIT_LIST_HEAD(&phy_ns->phys);
	mutex_init(&phy_ns->ns_lock);
}
