/* SPDX-License-Identifier: GPL-2.0 OR ISC */

#ifndef IPQESS_PORT_H
#define IPQESS_PORT_H

#include <net/gro_cells.h>
#include <net/devlink.h>

#include "ipqess_edma.h"
#include "ipqess_switch.h"

struct ipqess_bridge {
	struct net_device *netdev;
	refcount_t refcount;
};

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
	struct ipqess_bridge *bridge;
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

bool ipqess_port_recognize_netdev(const struct net_device *netdev);
bool ipqess_port_dev_is_foreign(const struct net_device *netdev,
				const struct net_device *foreign_netdev);

int ipqess_port_bridge_join(struct ipqess_port *port, struct net_device *br,
			    struct netlink_ext_ack *extack);
void ipqess_port_bridge_leave(struct ipqess_port *port, struct net_device *br);

int ipqess_port_attr_set(struct net_device *dev, const void *ctx,
			 const struct switchdev_attr *attr,
			 struct netlink_ext_ack *extack);

void ipqess_port_switchdev_event_work(struct work_struct *work);

int ipqess_port_check_8021q_upper(struct net_device *netdev,
				  struct netdev_notifier_changeupper_info *info);

struct net_device *ipqess_port_get_bridged_netdev(const struct ipqess_port *port);

int ipqess_port_obj_add(struct net_device *netdev, const void *ctx,
			const struct switchdev_obj *obj,
			struct netlink_ext_ack *extack);
int ipqess_port_obj_del(struct net_device *netdev, const void *ctx,
			const struct switchdev_obj *obj);

bool ipqess_port_offloads_bridge_port(struct ipqess_port *port,
				      const struct net_device *netdev);

/* Defined in ipqess_calib.c */
int psgmii_calibrate_and_test(struct qca8k_priv *priv);

#endif
