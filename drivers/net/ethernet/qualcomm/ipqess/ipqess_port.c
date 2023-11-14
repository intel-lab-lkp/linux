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

/* netdev ops */

static void ipqess_port_fast_age(const struct ipqess_port *port)
{
	struct qca8k_priv *priv = port->sw->priv;

	mutex_lock(&priv->reg_mutex);
	qca8k_fdb_access(priv, QCA8K_FDB_FLUSH_PORT, port->index);
	mutex_unlock(&priv->reg_mutex);
}

static void ipqess_port_stp_state_set(struct ipqess_port *port,
				      u8 state)
{
	struct qca8k_priv *priv = port->sw->priv;
	u32 stp_state;
	int err;

	switch (state) {
	case BR_STATE_DISABLED:
		stp_state = QCA8K_PORT_LOOKUP_STATE_DISABLED;
		break;
	case BR_STATE_BLOCKING:
		stp_state = QCA8K_PORT_LOOKUP_STATE_BLOCKING;
		break;
	case BR_STATE_LISTENING:
		stp_state = QCA8K_PORT_LOOKUP_STATE_LISTENING;
		break;
	case BR_STATE_LEARNING:
		stp_state = QCA8K_PORT_LOOKUP_STATE_LEARNING;
		break;
	case BR_STATE_FORWARDING:
	default:
		stp_state = QCA8K_PORT_LOOKUP_STATE_FORWARD;
		break;
	}

	err = qca8k_rmw(priv, QCA8K_PORT_LOOKUP_CTRL(port->index),
			QCA8K_PORT_LOOKUP_STATE_MASK, stp_state);

	if (err)
		dev_warn(priv->dev,
			 "failed to set STP state %d for port %d: err %d\n",
			 stp_state, port->index, err);
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

static int ipqess_port_do_vlan_add(struct qca8k_priv *priv, int port_index,
				   const struct switchdev_obj_port_vlan *vlan,
				   struct netlink_ext_ack *extack)
{
	bool untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;
	bool pvid = vlan->flags & BRIDGE_VLAN_INFO_PVID;
	int ret;

	ret = qca8k_vlan_add(priv, port_index, vlan->vid, untagged);
	if (ret) {
		dev_err(priv->dev, "Failed to add VLAN to port %d (%d)", port_index,
			ret);
		return ret;
	}

	if (pvid) {
		ret = qca8k_rmw(priv, QCA8K_EGRESS_VLAN(port_index),
				QCA8K_EGREES_VLAN_PORT_MASK(port_index),
				QCA8K_EGREES_VLAN_PORT(port_index, vlan->vid));
		if (ret)
			return ret;

		ret = qca8k_write(priv, QCA8K_REG_PORT_VLAN_CTRL0(port_index),
				  QCA8K_PORT_VLAN_CVID(vlan->vid) |
				  QCA8K_PORT_VLAN_SVID(vlan->vid));
	}

	return ret;
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
	ret = ipqess_port_do_vlan_add(port->sw->priv, port->index, &vlan, &extack);
	if (ret) {
		if (extack._msg)
			netdev_err(dev, "%s\n", extack._msg);
		return ret;
	}

	/* And CPU port... */
	ret = ipqess_port_do_vlan_add(port->sw->priv, 0, &vlan, &extack);
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
	int cnt = QCA8K_NUM_FDB_RECORDS;
	struct qca8k_fdb _fdb = { 0 };
	bool is_static;
	int ret = 0;

	mutex_lock(&priv->reg_mutex);
	while (cnt-- && !qca8k_fdb_next(priv, &_fdb, port->index)) {
		if (!_fdb.aging)
			break;
		is_static = (_fdb.aging == QCA8K_ATU_STATUS_STATIC);
		ret = ipqess_port_fdb_do_dump(_fdb.mac, _fdb.vid, is_static, &dump);
		if (ret)
			break;
	}
	mutex_unlock(&priv->reg_mutex);

	*idx = dump.idx;

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

