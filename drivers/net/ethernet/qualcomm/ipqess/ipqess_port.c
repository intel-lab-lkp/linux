// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Handling of a single switch port
 *
 * Copyright (c) 2023, Romain Gantois <romain.gantois@bootlin.com>
 * Based on net/dsa
 */

#include <linux/if_bridge.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/of_net.h>

#include "ipqess_port.h"
#include "ipqess_edma.h"
#include "ipqess_switch.h"
#include "ipqess_notifiers.h"

#define ipqess_port_from_pl_state(config, pl_config)\
container_of(config, struct ipqess_port, pl_config)

static struct device_type ipqess_port_type = {
	.name	= "switch",
};

struct net_device *ipqess_port_get_bridged_netdev(const struct ipqess_port *port)
{
	if (!port->bridge)
		return NULL;

	return port->netdev;
}

/* netdev ops */

static void ipqess_port_notify_bridge_fdb_flush(const struct ipqess_port *port,
						u16 vid)
{
	struct net_device *brport_dev = ipqess_port_get_bridged_netdev(port);
	struct switchdev_notifier_fdb_info info = {
		.vid = vid,
	};

	/* When the port becomes standalone it has already left the bridge.
	 * Don't notify the bridge in that case.
	 */
	if (!brport_dev)
		return;

	call_switchdev_notifiers(SWITCHDEV_FDB_FLUSH_TO_BRIDGE,
				 brport_dev, &info.info, NULL);
}

static void ipqess_port_fast_age(const struct ipqess_port *port)
{
	struct qca8k_priv *priv = port->sw->priv;

	qca8k_port_fast_age(priv, port->index);

	/* Flush all VLANs */
	ipqess_port_notify_bridge_fdb_flush(port, 0);
}

static void ipqess_port_stp_state_set(struct ipqess_port *port,
				      u8 state)
{
	struct qca8k_priv *priv = port->sw->priv;

	qca8k_port_stp_state_set(priv, port->index, state, false, false);
}

static void ipqess_port_set_state_now(struct ipqess_port *port,
				      u8 state, bool do_fast_age)
{
	ipqess_port_stp_state_set(port, state);

	if ((port->stp_state == BR_STATE_LEARNING ||
	     port->stp_state == BR_STATE_FORWARDING) &&
	    (state == BR_STATE_DISABLED || state == BR_STATE_BLOCKING ||
	    state == BR_STATE_LISTENING))
		ipqess_port_fast_age(port);

	port->stp_state = state;
}

static int ipqess_port_enable_rt(struct ipqess_port *port,
				 struct phy_device *phy)
{
	struct qca8k_priv *priv = port->sw->priv;

	qca8k_port_set_status(priv, port->index, 1);
	priv->port_enabled_map |= BIT(port->index);

	phy_support_asym_pause(phy);

	if (!port->bridge)
		ipqess_port_set_state_now(port, BR_STATE_FORWARDING, false);

	if (port->pl)
		phylink_start(port->pl);

	return 0;
}

static void ipqess_port_disable_rt(struct ipqess_port *port)
{
	struct qca8k_priv *priv = port->sw->priv;

	if (port->pl)
		phylink_stop(port->pl);

	if (!port->bridge)
		ipqess_port_set_state_now(port, BR_STATE_DISABLED, false);

	qca8k_port_set_status(priv, port->index, 0);
	priv->port_enabled_map &= ~BIT(port->index);
}

static int ipqess_port_open(struct net_device *netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct phy_device *phy = netdev->phydev;

	return ipqess_port_enable_rt(port, phy);
}

static int ipqess_port_close(struct net_device *netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);

	ipqess_port_disable_rt(port);

	return 0;
}

static netdev_tx_t ipqess_port_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);

	dev_sw_netstats_tx_add(netdev, 1, skb->len);

	memset(skb->cb, 0, sizeof(skb->cb));

	return ipqess_edma_xmit(skb, port->netdev);
}

static int ipqess_port_set_mac_address(struct net_device *netdev, void *a)
{
	struct sockaddr *addr = a;
	int err;

	/* If the port is down, the address isn't synced yet to hardware
	 * so there is nothing to change
	 */
	if (!(netdev->flags & IFF_UP)) {
		eth_hw_addr_set(netdev, addr->sa_data);
		return 0;
	}

	if (!ether_addr_equal(addr->sa_data, netdev->dev_addr)) {
		err = dev_uc_add(netdev, addr->sa_data);
		if (err < 0)
			return err;
	}

	return 0;
}

static int ipqess_port_ioctl(struct net_device *netdev, struct ifreq *ifr,
			     int cmd)
{
	struct ipqess_port *port = netdev_priv(netdev);

	return phylink_mii_ioctl(port->pl, ifr, cmd);
}

static int ipqess_port_get_iflink(const struct net_device *dev)
{
	return dev->ifindex;
}

static int ipqess_port_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ipqess_port *port = netdev_priv(dev);
	struct qca8k_priv *priv = port->sw->priv;
	int err;

	/* To change the MAX_FRAME_SIZE, the cpu port must be off
	 * or the switch panics.
	 */
	if (port->sw->port0_enabled)
		qca8k_port_set_status(priv, 0, 0);

	err = qca8k_write(priv, QCA8K_MAX_FRAME_SIZE, new_mtu +
			  ETH_HLEN + ETH_FCS_LEN);

	if (port->sw->port0_enabled)
		qca8k_port_set_status(priv, 0, 1);

	if (err)
		return err;

	dev->mtu = new_mtu;

	return 0;
}

static inline struct net_device *ipqess_port_bridge_dev_get(struct ipqess_port *port)
{
	return port->bridge ? port->bridge->netdev : NULL;
}

static int ipqess_port_vlan_rx_add_vid(struct net_device *dev, __be16 proto,
				       u16 vid)
{
	struct ipqess_port *port = netdev_priv(dev);
	struct switchdev_obj_port_vlan vlan = {
		.obj.id = SWITCHDEV_OBJ_ID_PORT_VLAN,
		.vid = vid,
		/* This API only allows programming tagged, non-PVID VIDs */
		.flags = 0,
	};
	struct netlink_ext_ack extack = {0};
	int ret;

	/* User port... */
	ret = qca8k_port_vlan_add(port->sw->priv, port->index, &vlan, &extack);
	if (ret) {
		if (extack._msg)
			netdev_err(dev, "%s\n", extack._msg);
		return ret;
	}

	/* And CPU port... */
	ret = qca8k_port_vlan_add(port->sw->priv, 0, &vlan, &extack);
	if (ret) {
		if (extack._msg)
			netdev_err(dev, "CPU port %d: %s\n", 0, extack._msg);
		return ret;
	}

	return 0;
}

static int ipqess_port_vlan_rx_kill_vid(struct net_device *dev, __be16 proto,
					u16 vid)
{
	struct ipqess_port *port = netdev_priv(dev);
	int err;

	err = qca8k_vlan_del(port->sw->priv, port->index, vid);
	if (err)
		return err;

	err = qca8k_vlan_del(port->sw->priv, 0, vid);
	if (err)
		return err;

	return 0;
}

static int
ipqess_port_fdb_do_dump(const unsigned char *addr, u16 vid,
			bool is_static, void *data)
{
	struct ipqess_port_dump_ctx *dump = data;
	u32 portid = NETLINK_CB(dump->cb->skb).portid;
	u32 seq = dump->cb->nlh->nlmsg_seq;
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;

	if (dump->idx < dump->cb->args[2])
		goto skip;

	nlh = nlmsg_put(dump->skb, portid, seq, RTM_NEWNEIGH,
			sizeof(*ndm), NLM_F_MULTI);
	if (!nlh)
		return -EMSGSIZE;

	ndm = nlmsg_data(nlh);
	ndm->ndm_family  = AF_BRIDGE;
	ndm->ndm_pad1    = 0;
	ndm->ndm_pad2    = 0;
	ndm->ndm_flags   = NTF_SELF;
	ndm->ndm_type    = 0;
	ndm->ndm_ifindex = dump->dev->ifindex;
	ndm->ndm_state   = is_static ? NUD_NOARP : NUD_REACHABLE;

	if (nla_put(dump->skb, NDA_LLADDR, ETH_ALEN, addr))
		goto nla_put_failure;

	if (vid && nla_put_u16(dump->skb, NDA_VLAN, vid))
		goto nla_put_failure;

	nlmsg_end(dump->skb, nlh);

skip:
	dump->idx++;
	return 0;

nla_put_failure:
	nlmsg_cancel(dump->skb, nlh);
	return -EMSGSIZE;
}

static int
ipqess_port_fdb_dump(struct sk_buff *skb, struct netlink_callback *cb,
		     struct net_device *dev, struct net_device *filter_dev,
		     int *idx)
{
	struct ipqess_port *port = netdev_priv(dev);
	struct qca8k_priv *priv = port->sw->priv;
	struct ipqess_port_dump_ctx dump = {
		.dev = dev,
		.skb = skb,
		.cb = cb,
		.idx = *idx,
	};
	int ret = 0;

	*idx = dump.idx;

	ret = qca8k_port_fdb_dump(priv, port->index, ipqess_port_fdb_do_dump,
				  &dump);

	return ret;
}

static const struct net_device_ops ipqess_port_netdev_ops = {
	.ndo_open               = ipqess_port_open,
	.ndo_stop               = ipqess_port_close,
	.ndo_set_mac_address    = ipqess_port_set_mac_address,
	.ndo_eth_ioctl          = ipqess_port_ioctl,
	.ndo_start_xmit         = ipqess_port_xmit,
	.ndo_get_iflink         = ipqess_port_get_iflink,
	.ndo_change_mtu         = ipqess_port_change_mtu,
	.ndo_vlan_rx_add_vid    = ipqess_port_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid   = ipqess_port_vlan_rx_kill_vid,
	.ndo_fdb_dump           = ipqess_port_fdb_dump,
};

/* Bridge ops */

static int ipqess_port_bridge_alloc(struct ipqess_port *port,
				    struct net_device *br,
				    struct netlink_ext_ack *extack)
{
	struct ipqess_bridge *bridge;

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return -ENOMEM;

	refcount_set(&bridge->refcount, 1);

	bridge->netdev = br;

	port->bridge = bridge;

	return 0;
}

/* Must be called under rcu_read_lock() */
static bool ipqess_port_can_apply_vlan_filtering(struct ipqess_port *port,
						 bool vlan_filtering,
						 struct netlink_ext_ack *extack)
{
	int err;

	/* VLAN awareness was off, so the question is "can we turn it on".
	 * We may have had 8021q uppers, those need to go. Make sure we don't
	 * enter an inconsistent state: deny changing the VLAN awareness state
	 * as long as we have 8021q uppers.
	 */
	if (vlan_filtering) {
		struct net_device *br = ipqess_port_bridge_dev_get(port);
		struct net_device *upper_dev, *netdev = port->netdev;
		struct list_head *iter;

		netdev_for_each_upper_dev_rcu(netdev, upper_dev, iter) {
			struct bridge_vlan_info br_info;
			u16 vid;

			if (!is_vlan_dev(upper_dev))
				continue;

			vid = vlan_dev_vlan_id(upper_dev);

			/* br_vlan_get_info() returns -EINVAL or -ENOENT if the
			 * device, respectively the VID is not found, returning
			 * 0 means success, which is a failure for us here.
			 */
			err = br_vlan_get_info(br, vid, &br_info);
			if (err == 0) {
				NL_SET_ERR_MSG_MOD(extack,
						   "Must first remove VLAN uppers having VIDs also present in bridge");
				return false;
			}
		}
	}

	/* VLAN filtering is not global so we can just return true here */
	return true;
}

static int ipqess_port_restore_vlan(struct net_device *vdev, int vid, void *arg)
{
	__be16 proto = vdev ? vlan_dev_vlan_proto(vdev) : htons(ETH_P_8021Q);

	return ipqess_port_vlan_rx_add_vid(arg, proto, vid);
}

static int ipqess_port_clear_vlan(struct net_device *vdev, int vid, void *arg)
{
	__be16 proto = vdev ? vlan_dev_vlan_proto(vdev) : htons(ETH_P_8021Q);

	return ipqess_port_vlan_rx_kill_vid(arg, proto, vid);
}

/* Keep the VLAN RX filtering list in sync with the hardware only if VLAN
 * filtering is enabled.
 */
static int ipqess_port_manage_vlan_filtering(struct net_device *netdev,
					     bool vlan_filtering)
{
	int err;

	if (vlan_filtering) {
		netdev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;

		err = vlan_for_each(netdev, ipqess_port_restore_vlan, netdev);
		if (err) {
			netdev_err(netdev,
				   "Failed to restore all VLAN's successfully, error %d\n",
				   err);
			vlan_for_each(netdev, ipqess_port_clear_vlan, netdev);
			netdev->features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;
			return err;
		}
	} else {
		err = vlan_for_each(netdev, ipqess_port_clear_vlan, netdev);
		if (err)
			return err;

		netdev->features &= ~NETIF_F_HW_VLAN_CTAG_FILTER;
	}

	return 0;
}

static int ipqess_port_vlan_filtering(struct ipqess_port *port,
				      bool vlan_filtering,
				      struct netlink_ext_ack *extack)
{
	bool old_vlan_filtering = port->vlan_filtering;
	bool apply;
	int err;

	/* We are called from ipqess_port_switchdev_blocking_event(),
	 * which is not under rcu_read_lock(), unlike
	 * ipqess_port_switchdev_event().
	 */
	rcu_read_lock();
	apply = ipqess_port_can_apply_vlan_filtering(port, vlan_filtering, extack);
	rcu_read_unlock();
	if (!apply)
		return -EINVAL;

	if (old_vlan_filtering == vlan_filtering)
		return 0;

	err = qca8k_port_vlan_filtering(port->sw->priv, port->index,
					vlan_filtering);

	if (err)
		return err;

	port->vlan_filtering = vlan_filtering;

	err = ipqess_port_manage_vlan_filtering(port->netdev,
						vlan_filtering);
	if (err)
		goto restore;

	return 0;

restore:
	err = qca8k_port_vlan_filtering(port->sw->priv, port->index,
					old_vlan_filtering);
	port->vlan_filtering = old_vlan_filtering;

	return err;
}

static void ipqess_port_reset_vlan_filtering(struct ipqess_port *port,
					     struct ipqess_bridge *bridge)
{
	struct netlink_ext_ack extack = {0};
	bool change_vlan_filtering = false;
	bool vlan_filtering;
	int err;

	if (br_vlan_enabled(bridge->netdev)) {
		change_vlan_filtering = true;
		vlan_filtering = false;
	}

	if (!change_vlan_filtering)
		return;

	err = ipqess_port_vlan_filtering(port, vlan_filtering, &extack);
	if (extack._msg) {
		dev_err(&port->netdev->dev, "port %d: %s\n", port->index,
			extack._msg);
	}
	if (err && err != -EOPNOTSUPP) {
		dev_err(&port->netdev->dev,
			"port %d failed to reset VLAN filtering to %d: %pe\n",
			port->index, vlan_filtering, ERR_PTR(err));
	}
}

static int ipqess_port_ageing_time(struct ipqess_port *port,
				   clock_t ageing_clock)
{
	unsigned long ageing_jiffies = clock_t_to_jiffies(ageing_clock);
	unsigned int ageing_time = jiffies_to_msecs(ageing_jiffies);

	if (ageing_time < IPQESS_SWITCH_AGEING_TIME_MIN ||
	    ageing_time > IPQESS_SWITCH_AGEING_TIME_MAX)
		return -ERANGE;

	/* Program the fastest ageing time in case of multiple bridges */
	ageing_time = ipqess_switch_fastest_ageing_time(port->sw, ageing_time);

	port->ageing_time = ageing_time;
	return ipqess_set_ageing_time(port->sw, ageing_time);
}

static int ipqess_port_switchdev_sync_attrs(struct ipqess_port *port,
					    struct netlink_ext_ack *extack)
{
	struct net_device *brport_dev = ipqess_port_get_bridged_netdev(port);
	struct net_device *br = ipqess_port_bridge_dev_get(port);
	int err;

	ipqess_port_set_state_now(port, br_port_get_stp_state(brport_dev), false);

	err = ipqess_port_vlan_filtering(port, br_vlan_enabled(br), extack);
	if (err)
		return err;

	err = ipqess_port_ageing_time(port, br_get_ageing_time(br));
	if (err && err != -EOPNOTSUPP)
		return err;

	return 0;
}

static void ipqess_port_switchdev_unsync_attrs(struct ipqess_port *port,
					       struct ipqess_bridge *bridge)
{
	/* Port left the bridge, put in BR_STATE_DISABLED by the bridge layer,
	 * so allow it to be in BR_STATE_FORWARDING to be kept functional
	 */
	ipqess_port_set_state_now(port, BR_STATE_FORWARDING, true);

	ipqess_port_reset_vlan_filtering(port, bridge);

	/* Ageing time is global to the switch chip, so don't change it
	 * here because we have no good reason (or value) to change it to.
	 */
}

static inline bool ipqess_port_offloads_bridge(struct ipqess_port *port,
					       const struct ipqess_bridge *bridge)
{
	return ipqess_port_bridge_dev_get(port) == bridge->netdev;
}

bool ipqess_port_offloads_bridge_port(struct ipqess_port *port,
				      const struct net_device *netdev)
{
	return ipqess_port_get_bridged_netdev(port) == netdev;
}

static inline bool
ipqess_port_offloads_bridge_dev(struct ipqess_port *port,
				const struct net_device *bridge_dev)
{
	/* QCA8K ports connected to a bridge, and event was emitted
	 * for the bridge.
	 */
	return ipqess_port_bridge_dev_get(port) == bridge_dev;
}

static void ipqess_port_bridge_destroy(struct ipqess_port *port,
				       const struct net_device *br)
{
	struct ipqess_bridge *bridge = port->bridge;

	port->bridge = NULL;

	if (!refcount_dec_and_test(&bridge->refcount))
		return;

	kfree(bridge);
}

int ipqess_port_bridge_join(struct ipqess_port *port, struct net_device *br,
			    struct netlink_ext_ack *extack)
{
	struct ipqess_switch *sw = port->sw;
	struct ipqess_bridge *bridge = NULL;
	struct qca8k_priv *priv = sw->priv;
	struct ipqess_port *other_port;
	struct net_device *brport_dev;
	int port_id = port->index;
	int port_mask = 0;
	int i, err;

	/* QCA8K doesn't support MST */
	if (br_mst_enabled(br)) {
		err = -EOPNOTSUPP;
		goto out_err;
	}

	/* Check if we already registered this bridge with
	 * another switch port
	 */
	for (i = 0; i < IPQESS_SWITCH_MAX_PORTS; i++) {
		other_port = sw->port_list[i];
		if (other_port && other_port->bridge &&
		    other_port->bridge->netdev == br)
			bridge = other_port->bridge;
	}

	if (bridge) {
		refcount_inc(&bridge->refcount);
		port->bridge = bridge;
	} else {
		err = ipqess_port_bridge_alloc(port, br, extack);
		if (err)
			goto out_err;
	}
	bridge = port->bridge;

	for (i = 1; i <= IPQESS_SWITCH_MAX_PORTS; i++) {
		other_port = sw->port_list[i - 1];
		if (!other_port || !ipqess_port_offloads_bridge(other_port, bridge))
			continue;
		/* Add this port to the portvlan mask of the other ports
		 * in the bridge
		 */
		err = regmap_set_bits(priv->regmap,
				      QCA8K_PORT_LOOKUP_CTRL(i),
				      BIT(port_id));
		if (err)
			goto out_rollback;
		if (i != port_id)
			port_mask |= BIT(i);
	}
	/* Also add the CPU port */
	err = regmap_set_bits(priv->regmap,
			      QCA8K_PORT_LOOKUP_CTRL(0),
			      BIT(port_id));
	port_mask |= BIT(0);

	/* Add all other ports to this ports portvlan mask */
	err = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port_id),
			QCA8K_PORT_LOOKUP_MEMBER, port_mask);
	if (err)
		goto out_rollback;

	brport_dev = ipqess_port_get_bridged_netdev(port);

	err = switchdev_bridge_port_offload(brport_dev, port->netdev, port,
					    &ipqess_switchdev_notifier,
					    &ipqess_switchdev_blocking_notifier,
					    false, extack);
	if (err)
		goto out_rollback_unbridge;

	err = ipqess_port_switchdev_sync_attrs(port, extack);
	if (err)
		goto out_rollback_unoffload;

	return 0;

out_rollback_unoffload:
	switchdev_bridge_port_unoffload(brport_dev, port,
					&ipqess_switchdev_notifier,
					&ipqess_switchdev_blocking_notifier);
	ipqess_flush_workqueue();
out_rollback_unbridge:
	for (i = 1; i <= IPQESS_SWITCH_MAX_PORTS; i++) {
		other_port = sw->port_list[i - 1];
		if (!other_port ||
		    !ipqess_port_offloads_bridge(other_port, port->bridge))
			continue;
		/* Remove this port from the portvlan mask of the other ports
		 * in the bridge
		 */
		regmap_clear_bits(priv->regmap,
				  QCA8K_PORT_LOOKUP_CTRL(i),
				  BIT(port_id));
	}

	/* Set the cpu port to be the only one in the portvlan mask of
	 * this port
	 */
	qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port_id),
		  QCA8K_PORT_LOOKUP_MEMBER, BIT(0));
out_rollback:
	ipqess_port_bridge_destroy(port, br);
out_err:
	dev_err(&port->netdev->dev, "Failed to join bridge: errno %d\n", err);
	return err;
}

void ipqess_port_bridge_leave(struct ipqess_port *port, struct net_device *br)
{
	struct ipqess_bridge *bridge = port->bridge;
	struct ipqess_switch *sw = port->sw;
	struct qca8k_priv *priv = sw->priv;
	struct ipqess_port *other_port;
	int port_id = port->index;
	int i;

	/* If the port could not be offloaded to begin with, then
	 * there is nothing to do.
	 */
	if (!bridge)
		return;

	for (i = 1; i <= IPQESS_SWITCH_MAX_PORTS; i++) {
		other_port = sw->port_list[i - 1];
		if (!other_port || !ipqess_port_offloads_bridge(other_port, bridge))
			continue;
		/* Remove this port from the portvlan mask of the other ports
		 * in the bridge
		 */
		regmap_clear_bits(priv->regmap,
				  QCA8K_PORT_LOOKUP_CTRL(i),
				  BIT(port_id));
	}

	/* Set the cpu port to be the only one in the portvlan mask of
	 * this port
	 */
	qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port_id),
		  QCA8K_PORT_LOOKUP_MEMBER, BIT(0));

	ipqess_port_switchdev_unsync_attrs(port, bridge);

	/* Here the port is already unbridged. Reflect the current configuration. */

	ipqess_port_bridge_destroy(port, br);
}

int ipqess_port_attr_set(struct net_device *dev, const void *ctx,
			 const struct switchdev_attr *attr,
			 struct netlink_ext_ack *extack)
{
	struct ipqess_port *port = netdev_priv(dev);
	int ret;

	if (ctx && ctx != port)
		return 0;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		if (!ipqess_port_offloads_bridge_port(port, attr->orig_dev))
			return -EOPNOTSUPP;

		ipqess_port_set_state_now(port, attr->u.stp_state, true);
		return 0;
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		if (!ipqess_port_offloads_bridge_dev(port, attr->orig_dev))
			return -EOPNOTSUPP;

		ret = ipqess_port_vlan_filtering(port, attr->u.vlan_filtering,
						 extack);
		break;
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		if (!ipqess_port_offloads_bridge_dev(port, attr->orig_dev))
			return -EOPNOTSUPP;

		ret = ipqess_port_ageing_time(port, attr->u.ageing_time);
		break;
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
		if (!ipqess_port_offloads_bridge_port(port, attr->orig_dev))
			return -EOPNOTSUPP;

		return -EINVAL;
	case SWITCHDEV_ATTR_ID_BRIDGE_MST:
	case SWITCHDEV_ATTR_ID_PORT_MST_STATE:
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
	case SWITCHDEV_ATTR_ID_VLAN_MSTI:
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

static int ipqess_port_vlan_check_for_8021q_uppers(struct net_device *netdev,
						   const struct switchdev_obj_port_vlan *vlan)
{
	struct net_device *upper_dev;
	struct list_head *iter;

	netdev_for_each_upper_dev_rcu(netdev, upper_dev, iter) {
		u16 vid;

		if (!is_vlan_dev(upper_dev))
			continue;

		vid = vlan_dev_vlan_id(upper_dev);
		if (vid == vlan->vid)
			return -EBUSY;
	}

	return 0;
}

static int ipqess_port_host_vlan_del(struct net_device *netdev,
				     const struct switchdev_obj *obj)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct net_device *br = ipqess_port_bridge_dev_get(port);
	struct switchdev_obj_port_vlan *vlan;

	/* Do nothing if this is a software bridge */
	if (!port->bridge)
		return -EOPNOTSUPP;

	if (br && !br_vlan_enabled(br))
		return 0;

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);

	return qca8k_vlan_del(port->sw->priv, 0, vlan->vid);
}

static int ipqess_port_vlan_del(struct net_device *netdev,
				const struct switchdev_obj *obj)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct net_device *br = ipqess_port_bridge_dev_get(port);
	struct qca8k_priv *priv = port->sw->priv;
	struct switchdev_obj_port_vlan *vlan;
	int ret;

	if (br && !br_vlan_enabled(br))
		return 0;

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);

	ret = qca8k_vlan_del(priv, port->index, vlan->vid);

	if (ret)
		dev_err(priv->dev, "Failed to delete VLAN from port %d (%d)\n",
			port->index, ret);

	return ret;
}

static int ipqess_port_host_vlan_add(struct net_device *netdev,
				     const struct switchdev_obj *obj,
				     struct netlink_ext_ack *extack)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct switchdev_obj_port_vlan *vlan;
	struct net_device *br;

	br = ipqess_port_bridge_dev_get(port);
	/* Do nothing is this is a software bridge */
	if (!port->bridge)
		return -EOPNOTSUPP;

	if (br && !br_vlan_enabled(br)) {
		NL_SET_ERR_MSG_MOD(extack, "skipping configuration of VLAN");
		return 0;
	}

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);

	vlan->flags &= ~BRIDGE_VLAN_INFO_PVID;

	/* Add vid to CPU port */
	return qca8k_port_vlan_add(port->sw->priv, 0, vlan, extack);
}

static int ipqess_port_vlan_add(struct net_device *netdev,
				const struct switchdev_obj *obj,
				struct netlink_ext_ack *extack)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct net_device *br = ipqess_port_bridge_dev_get(port);
	struct switchdev_obj_port_vlan *vlan;
	int err;

	if (br && !br_vlan_enabled(br)) {
		NL_SET_ERR_MSG_MOD(extack, "skipping configuration of VLAN");
		return 0;
	}

	vlan = SWITCHDEV_OBJ_PORT_VLAN(obj);

	/* Deny adding a bridge VLAN when there is already an 802.1Q upper with
	 * the same VID.
	 */
	if (br && br_vlan_enabled(br)) {
		rcu_read_lock();
		err = ipqess_port_vlan_check_for_8021q_uppers(netdev, vlan);
		rcu_read_unlock();
		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Port already has a VLAN upper with this VID");
			return err;
		}
	}

	err = qca8k_port_vlan_add(port->sw->priv, port->index, vlan, extack);
	return err;
}

static int ipqess_port_host_mdb_del(struct ipqess_port *port,
				    const struct switchdev_obj_port_mdb *mdb)
{
	struct qca8k_priv *priv = port->sw->priv;
	const u8 *addr = mdb->addr;
	u16 vid = mdb->vid;

	return qca8k_fdb_search_and_del(priv, BIT(0), addr, vid);
}

static int ipqess_port_host_mdb_add(struct ipqess_port *port,
				    const struct switchdev_obj_port_mdb *mdb)
{
	struct qca8k_priv *priv = port->sw->priv;
	const u8 *addr = mdb->addr;
	u16 vid = mdb->vid;

	return qca8k_fdb_search_and_insert(priv, BIT(0), addr, vid,
					   QCA8K_ATU_STATUS_STATIC);
}

static int ipqess_port_mdb_del(struct ipqess_port *port,
			       const struct switchdev_obj_port_mdb *mdb)
{
	struct qca8k_priv *priv = port->sw->priv;
	const u8 *addr = mdb->addr;
	u16 vid = mdb->vid;

	return qca8k_fdb_search_and_del(priv, BIT(port->index), addr, vid);
}

static int ipqess_port_mdb_add(struct ipqess_port *port,
			       const struct switchdev_obj_port_mdb *mdb)
{
	struct qca8k_priv *priv = port->sw->priv;
	const u8 *addr = mdb->addr;
	u16 vid = mdb->vid;

	return qca8k_fdb_search_and_insert(priv, BIT(port->index), addr, vid,
					   QCA8K_ATU_STATUS_STATIC);
}

int ipqess_port_obj_add(struct net_device *netdev, const void *ctx,
			const struct switchdev_obj *obj,
			struct netlink_ext_ack *extack)
{
	struct ipqess_port *port = netdev_priv(netdev);
	int err;

	if (ctx && ctx != port)
		return 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_MDB:
		if (!ipqess_port_offloads_bridge_port(port, obj->orig_dev))
			return -EOPNOTSUPP;

		err = ipqess_port_mdb_add(port, SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		if (!ipqess_port_offloads_bridge_dev(port, obj->orig_dev))
			return -EOPNOTSUPP;

		err = ipqess_port_host_mdb_add(port, SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		if (ipqess_port_offloads_bridge_port(port, obj->orig_dev))
			err = ipqess_port_vlan_add(netdev, obj, extack);
		else
			err = ipqess_port_host_vlan_add(netdev, obj, extack);
		break;
	case SWITCHDEV_OBJ_ID_MRP:
	case SWITCHDEV_OBJ_ID_RING_ROLE_MRP:
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

int ipqess_port_obj_del(struct net_device *netdev, const void *ctx,
			const struct switchdev_obj *obj)
{
	struct ipqess_port *port = netdev_priv(netdev);
	int err;

	if (ctx && ctx != port)
		return 0;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_MDB:
		if (!ipqess_port_offloads_bridge_port(port, obj->orig_dev))
			return -EOPNOTSUPP;

		err = ipqess_port_mdb_del(port, SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
	case SWITCHDEV_OBJ_ID_HOST_MDB:
		if (!ipqess_port_offloads_bridge_dev(port, obj->orig_dev))
			return -EOPNOTSUPP;

		err = ipqess_port_host_mdb_del(port, SWITCHDEV_OBJ_PORT_MDB(obj));
		break;
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		if (ipqess_port_offloads_bridge_port(port, obj->orig_dev))
			err = ipqess_port_vlan_del(netdev, obj);
		else
			err = ipqess_port_host_vlan_del(netdev, obj);
		break;
	case SWITCHDEV_OBJ_ID_MRP:
	case SWITCHDEV_OBJ_ID_RING_ROLE_MRP:
	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int ipqess_cpu_port_fdb_del(struct ipqess_port *port,
				   const unsigned char *addr, u16 vid)
{
	struct ipqess_mac_addr *mac_addr = NULL;
	struct ipqess_mac_addr *other_mac_addr;
	struct ipqess_switch *sw = port->sw;
	int err = 0;

	mutex_lock(&sw->addr_lists_lock);

	list_for_each_entry(other_mac_addr, &sw->fdbs, list)
		if (ether_addr_equal(other_mac_addr->addr, addr) && other_mac_addr->vid == vid)
			mac_addr = other_mac_addr;

	if (!mac_addr) {
		err = -ENOENT;
		goto out;
	}

	if (!refcount_dec_and_test(&mac_addr->refcount))
		goto out;

	err = qca8k_fdb_del(sw->priv, addr, BIT(IPQESS_SWITCH_CPU_PORT), vid);
	if (err) {
		refcount_set(&mac_addr->refcount, 1);
		goto out;
	}

	list_del(&mac_addr->list);
	kfree(mac_addr);

out:
	mutex_unlock(&sw->addr_lists_lock);

	return err;
}

static int ipqess_cpu_port_fdb_add(struct ipqess_port *port,
				   const unsigned char *addr, u16 vid)
{
	struct ipqess_switch *sw = port->sw;
	struct ipqess_mac_addr *other_a = NULL;
	struct ipqess_mac_addr *a = NULL;
	int err = 0;

	mutex_lock(&sw->addr_lists_lock);

	list_for_each_entry(other_a, &sw->fdbs, list)
		if (ether_addr_equal(other_a->addr, addr) && other_a->vid == vid)
			a = other_a;

	if (a) {
		refcount_inc(&a->refcount);
		goto out;
	}

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a) {
		err = -ENOMEM;
		goto out;
	}

	err = qca8k_port_fdb_insert(port->sw->priv, addr,
				    BIT(IPQESS_SWITCH_CPU_PORT), vid);
	if (err) {
		kfree(a);
		goto out;
	}

	ether_addr_copy(a->addr, addr);
	a->vid = vid;
	refcount_set(&a->refcount, 1);
	list_add_tail(&a->list, &sw->fdbs);

out:
	mutex_unlock(&sw->addr_lists_lock);

	return err;
}

static void
ipqess_fdb_offload_notify(struct ipqess_switchdev_event_work *switchdev_work)
{
	struct switchdev_notifier_fdb_info info = {};

	info.addr = switchdev_work->addr;
	info.vid = switchdev_work->vid;
	info.offloaded = true;
	call_switchdev_notifiers(SWITCHDEV_FDB_OFFLOADED,
				 switchdev_work->orig_netdev, &info.info, NULL);
}

void ipqess_port_switchdev_event_work(struct work_struct *work)
{
	struct ipqess_switchdev_event_work *switchdev_work =
		container_of(work, struct ipqess_switchdev_event_work, work);
	struct net_device *netdev = switchdev_work->netdev;
	const unsigned char *addr = switchdev_work->addr;
	struct ipqess_port *port = netdev_priv(netdev);
	struct ipqess_switch *sw = port->sw;
	struct qca8k_priv *priv = sw->priv;
	u16 vid = switchdev_work->vid;
	int err;

	if (!vid)
		vid = QCA8K_PORT_VID_DEF;

	switch (switchdev_work->event) {
	case SWITCHDEV_FDB_ADD_TO_DEVICE:
		if (switchdev_work->host_addr)
			err = ipqess_cpu_port_fdb_add(port, addr, vid);
		else
			err = qca8k_port_fdb_insert(priv, addr, BIT(port->index), vid);
		if (err) {
			dev_err(&port->netdev->dev,
				"port %d failed to add %pM vid %d to fdb: %d\n",
				port->index, addr, vid, err);
			break;
		}
		ipqess_fdb_offload_notify(switchdev_work);
		break;

	case SWITCHDEV_FDB_DEL_TO_DEVICE:
		if (switchdev_work->host_addr)
			err = ipqess_cpu_port_fdb_del(port, addr, vid);
		else
			err = qca8k_fdb_del(priv, addr, BIT(port->index), vid);
		if (err) {
			dev_err(&port->netdev->dev,
				"port %d failed to delete %pM vid %d from fdb: %d\n",
				port->index, addr, vid, err);
		}

		break;
	}

	kfree(switchdev_work);
}

int ipqess_port_check_8021q_upper(struct net_device *netdev,
				  struct netdev_notifier_changeupper_info *info)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct net_device *br = ipqess_port_bridge_dev_get(port);
	struct bridge_vlan_info br_info;
	struct netlink_ext_ack *extack;
	int err = NOTIFY_DONE;
	u16 vid;

	if (!br || !br_vlan_enabled(br))
		return NOTIFY_DONE;

	extack = netdev_notifier_info_to_extack(&info->info);
	vid = vlan_dev_vlan_id(info->upper_dev);

	/* br_vlan_get_info() returns -EINVAL or -ENOENT if the
	 * device, respectively the VID is not found, returning
	 * 0 means success, which is a failure for us here.
	 */
	err = br_vlan_get_info(br, vid, &br_info);
	if (err == 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "This VLAN is already configured by the bridge");
		return notifier_from_errno(-EBUSY);
	}

	return NOTIFY_DONE;
}

/* phylink ops */

static void
ipqess_phylink_mac_config(struct phylink_config *config,
			  unsigned int mode,
			  const struct phylink_link_state *state)
{
	struct ipqess_port *port = ipqess_port_from_pl_state(config, pl_config);
	struct qca8k_priv *priv = port->sw->priv;

	switch (port->index) {
	case 0:
		/* CPU port, no configuration needed */
		return;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
		if (phy_interface_mode_is_rgmii(state->interface))
			regmap_set_bits(priv->regmap,
					QCA8K_IPQ4019_REG_RGMII_CTRL,
					QCA8K_IPQ4019_RGMII_CTRL_CLK);
		return;
	default:
		dev_err(priv->dev, "%s: unsupported port: %i\n", __func__,
			port->index);
		return;
	}
}

static void
ipqess_phylink_mac_link_down(struct phylink_config *config,
			     unsigned int mode,
			     phy_interface_t interface)
{
	struct ipqess_port *port = ipqess_port_from_pl_state(config, pl_config);
	struct qca8k_priv *priv = port->sw->priv;

	qca8k_port_set_status(priv, port->index, 0);
}

static void ipqess_phylink_mac_link_up(struct phylink_config *config,
				       struct phy_device *phydev,
				       unsigned int mode,
				       phy_interface_t interface,
				       int speed, int duplex,
				       bool tx_pause, bool rx_pause)
{
	struct ipqess_port *port = ipqess_port_from_pl_state(config, pl_config);
	struct qca8k_priv *priv = port->sw->priv;
	u32 reg;

	if (phylink_autoneg_inband(mode)) {
		reg = QCA8K_PORT_STATUS_LINK_AUTO;
	} else {
		switch (speed) {
		case SPEED_10:
			reg = QCA8K_PORT_STATUS_SPEED_10;
			break;
		case SPEED_100:
			reg = QCA8K_PORT_STATUS_SPEED_100;
			break;
		case SPEED_1000:
			reg = QCA8K_PORT_STATUS_SPEED_1000;
			break;
		default:
			reg = QCA8K_PORT_STATUS_LINK_AUTO;
			break;
		}

		if (duplex == DUPLEX_FULL)
			reg |= QCA8K_PORT_STATUS_DUPLEX;

		if (rx_pause || port->index == 0)
			reg |= QCA8K_PORT_STATUS_RXFLOW;

		if (tx_pause || port->index == 0)
			reg |= QCA8K_PORT_STATUS_TXFLOW;
	}

	reg |= QCA8K_PORT_STATUS_TXMAC | QCA8K_PORT_STATUS_RXMAC;

	qca8k_write(priv, QCA8K_REG_PORT_STATUS(port->index), reg);
}

static const struct phylink_mac_ops ipqess_phylink_mac_ops = {
	.mac_config = ipqess_phylink_mac_config,
	.mac_link_down = ipqess_phylink_mac_link_down,
	.mac_link_up = ipqess_phylink_mac_link_up,
};

static int ipqess_phylink_create(struct net_device *netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct phylink_config *pl_config = &port->pl_config;
	phy_interface_t mode;
	struct phylink *pl;
	int err;

	err = of_get_phy_mode(port->dn, &mode);
	if (err)
		mode = PHY_INTERFACE_MODE_NA;

	switch (port->index) {
	case 1:
	case 2:
	case 3:
		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  pl_config->supported_interfaces);
		break;
	case 4:
	case 5:
		phy_interface_set_rgmii(pl_config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_PSGMII,
			  pl_config->supported_interfaces);
		break;
	case 0: /* CPU port, this shouldn't happen */
	default:
		return -EINVAL;
	}
	/* phylink caps */
	pl_config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
		MAC_10 | MAC_100 | MAC_1000FD;

	pl = phylink_create(pl_config, of_fwnode_handle(port->dn),
			    mode, &ipqess_phylink_mac_ops);
	if (IS_ERR(pl))
		return PTR_ERR(pl);

	port->pl = pl;
	return 0;
}

static int ipqess_port_phy_connect(struct net_device *netdev, int addr,
				   u32 flags)
{
	struct ipqess_port *port = netdev_priv(netdev);

	netdev->phydev = mdiobus_get_phy(port->mii_bus, addr);
	if (!netdev->phydev) {
		netdev_err(netdev, "no phy at %d\n", addr);
		return -ENODEV;
	}

	netdev->phydev->dev_flags |= flags;

	return phylink_connect_phy(port->pl, netdev->phydev);
}

static int ipqess_port_phy_setup(struct net_device *netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct device_node *port_dn = port->dn;
	u32 phy_flags = 0;
	int ret;

	port->pl_config.dev = &netdev->dev;
	port->pl_config.type = PHYLINK_NETDEV;

	ret = ipqess_phylink_create(netdev);
	if (ret)
		return ret;

	ret = phylink_of_phy_connect(port->pl, port_dn, phy_flags);
	if (ret == -ENODEV && port->mii_bus) {
		/* We could not connect to a designated PHY or SFP, so try to
		 * use the switch internal MDIO bus instead
		 */
		ret = ipqess_port_phy_connect(netdev, port->index, phy_flags);
	}

	if (ret) {
		netdev_err(netdev, "failed to connect to PHY: %pe\n",
			   ERR_PTR(ret));
		phylink_destroy(port->pl);
		port->pl = NULL;
	}

	dev_info(&netdev->dev, "enabled port's phy: %s",
		 phydev_name(netdev->phydev));
	return ret;
}

/* netlink */

#define IFLA_IPQESS_UNSPEC 0
#define IFLA_IPQESS_MAX 0

static const struct nla_policy ipqess_port_policy[IFLA_IPQESS_MAX + 1] = {
	[IFLA_IPQESS_MAX] = { .type = NLA_U32 },
};

static size_t ipqess_port_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(u32));
}

static int ipqess_port_fill_info(struct sk_buff *skb,
				 const struct net_device *dev)
{
	if (nla_put_u32(skb, IFLA_IPQESS_UNSPEC, dev->ifindex))
		return -EMSGSIZE;

	return 0;
}

static struct rtnl_link_ops ipqess_port_link_ops __read_mostly = {
	.kind         = "switch",
	.priv_size    = sizeof(struct ipqess_port),
	.maxtype      = 1,
	.policy       = ipqess_port_policy,
	.get_size     = ipqess_port_get_size,
	.fill_info    = ipqess_port_fill_info,
	.netns_refund = true,
};

/* devlink */

static int ipqess_port_devlink_setup(struct ipqess_port *port)
{
	struct devlink_port *dlp = &port->devlink_port;
	struct devlink *dl = port->sw->devlink;
	struct devlink_port_attrs attrs = {};
	const unsigned char *id;
	unsigned int index = 0;
	unsigned char len;
	int err;

	id = (const unsigned char *)&index;
	len = sizeof(index);
	memset(dlp, 0, sizeof(*dlp));

	attrs.phys.port_number = port->index;
	memcpy(attrs.switch_id.id, id, len);
	attrs.switch_id.id_len = len;
	attrs.flavour = DEVLINK_PORT_FLAVOUR_PHYSICAL;
	devlink_port_attrs_set(dlp, &attrs);

	err = devlink_port_register(dl, dlp, port->index);
	if (err)
		return err;

	return 0;
}

/* register */

int ipqess_port_register(struct ipqess_switch *sw,
			 struct device_node *port_node)
{
	struct qca8k_priv *priv = sw->priv;
	struct net_device *netdev;
	struct ipqess_port *port;
	const char *name;
	int assign_type;
	int num_queues;
	u32 index;
	int err;

	err = of_property_read_u32(port_node, "reg", &index);
	if (err) {
		pr_err("Node without reg property!");
		return err;
	}

	name = of_get_property(port_node, "label", NULL);
	if (!name) {
		name = "eth%d";
		assign_type = NET_NAME_ENUM;
	} else {
		assign_type = NET_NAME_PREDICTABLE;
	}

	/* For the NAPI leader, we allocate one queue per MAC queue */
	if (!sw->napi_leader)
		num_queues = IPQESS_EDMA_NETDEV_QUEUES;
	else
		num_queues = 1;

	netdev = alloc_netdev_mqs(sizeof(struct ipqess_port), name, assign_type,
				  ether_setup, num_queues, num_queues);
	if (!netdev)
		return -ENOMEM;

	if (!sw->napi_leader)
		sw->napi_leader = netdev;

	port = netdev_priv(netdev);
	port->index = (int)index;
	port->dn = port_node;
	port->netdev = netdev;
	port->edma = NULL; /* Assigned during edma initialization */
	port->qid = port->index - 1;
	port->sw = sw;
	port->bridge = NULL;

	of_get_mac_address(port_node, port->mac);
	if (!is_zero_ether_addr(port->mac))
		eth_hw_addr_set(netdev, port->mac);
	else
		eth_hw_addr_random(netdev);

	netdev->netdev_ops = &ipqess_port_netdev_ops;
	netdev->max_mtu = QCA8K_MAX_MTU;
	SET_NETDEV_DEVTYPE(netdev, &ipqess_port_type);
	SET_NETDEV_DEV(netdev, priv->dev);
	SET_NETDEV_DEVLINK_PORT(netdev, &port->devlink_port);
	netdev->dev.of_node = port->dn;

	netdev->rtnl_link_ops = &ipqess_port_link_ops;
	ipqess_port_set_ethtool_ops(netdev);

	netdev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!netdev->tstats) {
		free_netdev(netdev);
		return -ENOMEM;
	}

	err = ipqess_port_devlink_setup(port);
	if (err)
		goto out_free;

	err = gro_cells_init(&port->gcells, netdev);
	if (err)
		goto out_devlink;

	err = ipqess_port_phy_setup(netdev);
	if (err) {
		pr_err("error setting up PHY: %d\n", err);
		goto out_gcells;
	}

	/* We use the qid and not the index because port 0 isn't registered */
	sw->port_list[port->qid] = port;

	err = register_netdev(netdev);
	if (err) {
		pr_err("error %d registering interface %s\n",
		       err, netdev->name);
		rtnl_unlock();
		goto out_phy;
	}

	return 0;

out_phy:
	rtnl_lock();
	phylink_disconnect_phy(port->pl);
	rtnl_unlock();
	phylink_destroy(port->pl);
	port->pl = NULL;
out_gcells:
	gro_cells_destroy(&port->gcells);
out_devlink:
	devlink_port_unregister(&port->devlink_port);
out_free:
	free_percpu(netdev->tstats);
	free_netdev(netdev);
	sw->port_list[port->qid] = NULL;
	return err;
}

void ipqess_port_unregister(struct ipqess_port *port)
{
	struct net_device *netdev = port->netdev;

	unregister_netdev(netdev);

	devlink_port_unregister(&port->devlink_port);

	rtnl_lock();
	phylink_disconnect_phy(port->pl);
	rtnl_unlock();
	phylink_destroy(port->pl);
	port->pl = NULL;

	gro_cells_destroy(&port->gcells);

	free_percpu(netdev->tstats);
	free_netdev(netdev);
}

/* Utilities */

/* Returns true if any port of this switch offloads the given net_device */
static bool ipqess_switch_offloads_bridge_port(struct ipqess_switch *sw,
					       const struct net_device *netdev)
{
	struct ipqess_port *port;
	int i;

	for (i = 0; i < IPQESS_SWITCH_MAX_PORTS; i++) {
		port = sw->port_list[i];
		if (port && ipqess_port_offloads_bridge_port(port, netdev))
			return true;
	}

	return false;
}

/* Returns true if any port of this switch offloads the given bridge */
static inline bool
ipqess_switch_offloads_bridge_dev(struct ipqess_switch *sw,
				  const struct net_device *bridge_dev)
{
	struct ipqess_port *port;
	int i;

	for (i = 0; i < IPQESS_SWITCH_MAX_PORTS; i++) {
		port = sw->port_list[i];
		if (port && ipqess_port_offloads_bridge_dev(port, bridge_dev))
			return true;
	}

	return false;
}

bool ipqess_port_recognize_netdev(const struct net_device *netdev)
{
	return netdev->netdev_ops == &ipqess_port_netdev_ops;
}

bool ipqess_port_dev_is_foreign(const struct net_device *netdev,
				const struct net_device *foreign_netdev)
{
	struct ipqess_port *port = netdev_priv(netdev);
	struct ipqess_switch *sw = port->sw;

	if (netif_is_bridge_master(foreign_netdev))
		return !ipqess_switch_offloads_bridge_dev(sw, foreign_netdev);

	if (netif_is_bridge_port(foreign_netdev))
		return !ipqess_switch_offloads_bridge_port(sw, foreign_netdev);

	/* Everything else is foreign */
	return true;
}
