/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/ethtool.h>
#include <linux/types.h>

#ifndef __PHY_PORT_H
#define __PHY_PORT_H

struct phy_port;

/**
 * enum phy_port_parent - The device this port is attached to
 *
 * @PHY_PORT_PHY: Indicates that the port is driven by a PHY device
 */
enum phy_port_parent {
	PHY_PORT_PHY,
};

/**
 * struct phy_port - A representation of a network device physical interface
 *
 * @head: Used by the port's parent to list ports
 * @parent_type: The type of device this port is directly connected to
 * @phy: If the parent is PHY_PORT_PHYDEV, the PHY controlling that port
 * @lanes: The number of lanes (diff pairs) this port has, 0 if not applicable
 * @medium: The physical medium this port provides access to
 * @supported: The link modes this port can expose
 * @active: Indicates if the port is currently part of the active link.
 */
struct phy_port {
	struct list_head head;
	enum phy_port_parent parent_type;
	union {
		struct phy_device *phy;
	};

	int lanes;
	unsigned long mediums;
	__ETHTOOL_DECLARE_LINK_MODE_MASK(supported);

	bool active;
};

struct phy_port *phy_port_alloc(void);
void phy_port_destroy(struct phy_port *port);

static inline struct phy_device *port_phydev(struct phy_port *port)
{
	return port->phy;
}

struct phy_port *phy_of_parse_port(struct device_node *dn);

static inline bool phy_port_is_copper(struct phy_port *port)
{
	return port->mediums == BIT(ETHTOOL_LINK_MEDIUM_BASET);
}

static inline bool phy_port_is_fiber(struct phy_port *port)
{
	return !!(port->mediums & ETHTOOL_MEDIUM_FIBER_BITS);
}

void phy_port_update_supported(struct phy_port *port);

int phy_port_get_type(struct phy_port *port);

#endif
