/* SPDX-License-Identifier: GPL-2.0 OR ISC */

#ifndef IPQESS_PORT_H
#define IPQESS_PORT_H

#include <net/gro_cells.h>
#include <net/devlink.h>

#include "ipqess_edma.h"
#include "ipqess_switch.h"

struct ipqess_port {
	u16 index;
	u16 qid;

	struct ipqess_edma *edma;
	struct ipqess_switch *sw;
	struct phylink *pl;
	struct phylink_config pl_config;
	struct device_node *dn;
	struct mii_bus *mii_bus;
	struct net_device *netdev;
	struct devlink_port devlink_port;

	u8       stp_state;

	u8       mac[ETH_ALEN];

	/* Warning: the following bit field is not atomic, and updating it
	 * can only be done from code paths where concurrency is not possible
	 * (probe time or under rtnl_lock).
	 */
	u8			vlan_filtering:1;

	unsigned int		ageing_time;

	struct gro_cells	gcells;

#ifdef CONFIG_NET_POLL_CONTROLLER
	struct netpoll		*netpoll;
#endif
};

struct ipqess_port_dump_ctx {
	struct net_device *dev;
	struct sk_buff *skb;
	struct netlink_callback *cb;
	int idx;
};

struct ipqess_mac_addr {
	unsigned char addr[ETH_ALEN];
	u16 vid;
	refcount_t refcount;
	struct list_head list;
};

int ipqess_port_register(struct ipqess_switch *sw,
			 struct device_node *port_node);
void ipqess_port_unregister(struct ipqess_port *port);

/* Defined in ipqess_ethtool.c */
void ipqess_port_set_ethtool_ops(struct net_device *netdev);

#endif
