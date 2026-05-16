// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IEEE 802.1Q-2014 §6.7.1 Two-Port MAC Relay driver
 *
 * A TPMR is a minimal L2 relay between exactly two member ports: no FDB,
 * no MAC learning, no STP. Frames received on one member are forwarded
 * unconditionally out the other, including the IEEE-reserved
 * 01:80:c2:00:00:0x group (LACPDUs, LLDP, EAPOL, ...) except for
 * 01:80:c2:00:00:01 (PAUSE), which is MAC-terminated by spec.
 *
 * Inspired by OpenBSD's tpmr(4) (dlg@, 2019), reimplemented against
 * Linux's rx_handler / rtnl_link_ops infrastructure.
 */

#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/if_link.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/u64_stats_sync.h>
#include <net/rtnetlink.h>

#define TPMR_DRV_NAME         "tpmr"
#define TPMR_DRV_VERSION      "0.1"
#define TPMR_MAX_PORTS                2

struct tpmr_priv;

struct tpmr_port {
	struct net_device __rcu         *dev;
	struct tpmr_priv                *tpmr;
	unsigned int                    slot;
};

struct tpmr_priv {
	struct net_device               *dev;
	struct tpmr_port                ports[TPMR_MAX_PORTS];
	unsigned int                    n_ports;
};

static const struct net_device_ops tpmr_netdev_ops;
static struct rtnl_link_ops tpmr_link_ops __read_mostly;
static int tpmr_device_event(struct notifier_block *nb,
			     unsigned long event, void *ptr);

static struct notifier_block tpmr_notifier_block __read_mostly = {
	.notifier_call = tpmr_device_event,
};

/*
 * IEEE 802.1Q-2014 §8.6.3 Table 8-1 — a TPMR forwards every reserved
 * 01:80:c2:00:00:0x address except 01:80:c2:00:00:01 (IEEE 802.3x PAUSE),
 * which is MAC-terminated by spec. Forwarding LACPDUs (02), 802.1X PAE
 * (03), LLDP (0e), etc. end-to-end is the whole point of this driver.
 */
static bool tpmr_should_relay_reserved(const u8 *dst)
{
	static const u8 prefix[5] = { 0x01, 0x80, 0xc2, 0x00, 0x00 };

	if (memcmp(dst, prefix, sizeof(prefix)) != 0)
		return true;

	return dst[5] != 0x01;
}

static rx_handler_result_t tpmr_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct net_device *peer_dev;
	struct tpmr_port *p;
	struct ethhdr *eh;
	unsigned int len;

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (unlikely(!skb))
		return RX_HANDLER_CONSUMED;
	*pskb = skb;

	p = rcu_dereference(skb->dev->rx_handler_data);
	if (unlikely(!p))
		return RX_HANDLER_PASS;

	peer_dev = rcu_dereference(p->tpmr->ports[!p->slot].dev);
	if (!peer_dev || !netif_running(peer_dev) ||
	    !netif_carrier_ok(peer_dev))
		goto drop;

	eh = eth_hdr(skb);
	if (is_multicast_ether_addr(eh->h_dest) &&
	    !tpmr_should_relay_reserved(eh->h_dest))
		return RX_HANDLER_PASS;

	/* eth_type_trans() pulled the L2 header on receive; push it back
	 * before dev_queue_xmit().
	 */
	skb_push(skb, ETH_HLEN);
	skb->dev = peer_dev;

	len = skb->len;
	dev_sw_netstats_rx_add(p->tpmr->dev, len);

	if (dev_queue_xmit(skb) == NET_XMIT_SUCCESS)
		dev_sw_netstats_tx_add(p->tpmr->dev, 1, len);

	return RX_HANDLER_CONSUMED;

drop:
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
}

/* Master carrier is the logical AND of both slaves' carriers, and we only
 * advertise it when both slots are populated. Called with RTNL held.
 */
static void tpmr_update_carrier(struct tpmr_priv *tpmr)
{
	struct net_device *a, *b;
	bool up;

	ASSERT_RTNL();

	a = rtnl_dereference(tpmr->ports[0].dev);
	b = rtnl_dereference(tpmr->ports[1].dev);

	up = a && b && netif_carrier_ok(a) && netif_carrier_ok(b);

	if (up)
		netif_carrier_on(tpmr->dev);
	else
		netif_carrier_off(tpmr->dev);
}

static int tpmr_add_slave(struct net_device *dev, struct net_device *slave_dev,
			  struct netlink_ext_ack *extack)
{
	struct tpmr_priv *tpmr = netdev_priv(dev);
	struct tpmr_port *port = NULL;
	unsigned int slot;
	int err;

	ASSERT_RTNL();

	if (slave_dev->type != ARPHRD_ETHER) {
		NL_SET_ERR_MSG(extack, "Only Ethernet devices can be TPMR members");
		return -EINVAL;
	}
	if (slave_dev->flags & IFF_LOOPBACK) {
		NL_SET_ERR_MSG(extack, "Loopback cannot be a TPMR member");
		return -EINVAL;
	}
	if (netdev_is_rx_handler_busy(slave_dev)) {
		NL_SET_ERR_MSG(extack,
			       "Device already has an rx_handler (bridge/bond/etc.)");
		return -EBUSY;
	}
	if (tpmr->n_ports >= TPMR_MAX_PORTS) {
		NL_SET_ERR_MSG(extack, "TPMR already has two members");
		return -EBUSY;
	}

	/* Enforce MTU consistency between the two members. */
	if (tpmr->n_ports == 1) {
		struct net_device *first;

		first = rtnl_dereference(tpmr->ports[tpmr->ports[0].slot].dev);
		if (first && first->mtu != slave_dev->mtu) {
			NL_SET_ERR_MSG(extack,
				       "Member MTU must match the other member's");
			return -EINVAL;
		}
	}

	for (slot = 0; slot < TPMR_MAX_PORTS; slot++) {
		if (!rtnl_dereference(tpmr->ports[slot].dev)) {
			port = &tpmr->ports[slot];
			break;
		}
	}
	if (!port)
		return -EBUSY;  /* should not happen given n_ports check */

	port->tpmr = tpmr;
	port->slot = slot;

	err = dev_set_promiscuity(slave_dev, 1);
	if (err)
		return err;

	err = netdev_master_upper_dev_link(slave_dev, dev, NULL, NULL, extack);
	if (err)
		goto err_promisc;

	err = netdev_rx_handler_register(slave_dev, tpmr_handle_frame, port);
	if (err)
		goto err_upper;

	rcu_assign_pointer(port->dev, slave_dev);
	tpmr->n_ports++;

	/* Match dev->mtu to the members once a first one attaches. */
	if (tpmr->n_ports == 1)
		dev->mtu = slave_dev->mtu;

	tpmr_update_carrier(tpmr);
	return 0;

err_upper:
	netdev_upper_dev_unlink(slave_dev, dev);
err_promisc:
	dev_set_promiscuity(slave_dev, -1);
	return err;
}

static int tpmr_del_slave(struct net_device *dev, struct net_device *slave_dev)
{
	struct tpmr_priv *tpmr = netdev_priv(dev);
	struct tpmr_port *port = NULL;
	unsigned int slot;

	ASSERT_RTNL();

	for (slot = 0; slot < TPMR_MAX_PORTS; slot++) {
		if (rtnl_dereference(tpmr->ports[slot].dev) == slave_dev) {
			port = &tpmr->ports[slot];
			break;
		}
	}
	if (!port)
		return -ENOENT;

	netdev_rx_handler_unregister(slave_dev);
	RCU_INIT_POINTER(port->dev, NULL);
	netdev_upper_dev_unlink(slave_dev, dev);
	dev_set_promiscuity(slave_dev, -1);

	tpmr->n_ports--;
	tpmr_update_carrier(tpmr);
	return 0;
}

static int tpmr_device_event(struct notifier_block *nb,
			     unsigned long event, void *ptr)
{
	struct net_device *slave_dev = netdev_notifier_info_to_dev(ptr);
	struct net_device *master;
	struct tpmr_priv *tpmr;

	if (slave_dev->reg_state == NETREG_REGISTERED) {
		master = netdev_master_upper_dev_get(slave_dev);
		if (!master || master->rtnl_link_ops != &tpmr_link_ops)
			return NOTIFY_DONE;
	} else {
		return NOTIFY_DONE;
	}

	tpmr = netdev_priv(master);

	switch (event) {
	case NETDEV_CHANGE:
	case NETDEV_UP:
	case NETDEV_DOWN:
		tpmr_update_carrier(tpmr);
		break;
	case NETDEV_UNREGISTER:
		tpmr_del_slave(master, slave_dev);
		break;
	}
	return NOTIFY_DONE;
}

static netdev_tx_t tpmr_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	/* Master itself doesn't transmit; frames enter via slave rx and
	 * exit via the peer slave's tx. Drop and count.
	 */
	kfree_skb(skb);
	dev_core_stats_tx_dropped_inc(dev);
	return NETDEV_TX_OK;
}

static void tpmr_get_drvinfo(struct net_device *dev,
			     struct ethtool_drvinfo *drvinfo)
{
	strscpy(drvinfo->driver, TPMR_DRV_NAME, sizeof(drvinfo->driver));
	strscpy(drvinfo->version, TPMR_DRV_VERSION, sizeof(drvinfo->version));
}

static const struct ethtool_ops tpmr_ethtool_ops = {
	.get_drvinfo    = tpmr_get_drvinfo,
	.get_link       = ethtool_op_get_link,
};

static void tpmr_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops         = &tpmr_netdev_ops;
	dev->ethtool_ops        = &tpmr_ethtool_ops;
	dev->needs_free_netdev  = true;

	/* No qdisc/queue on the master — we never xmit through it. */
	dev->priv_flags         |= IFF_NO_QUEUE;
	dev->priv_flags         |= IFF_NO_RX_HANDLER;

	/* Cosmetic: a relay is not a multicast endpoint of its own. */
	dev->flags              &= ~IFF_MULTICAST;

	dev->lltx               = true;
	dev->hw_features        = 0;

	eth_hw_addr_random(dev);
}

static int tpmr_newlink(struct net_device *dev,
		struct rtnl_newlink_params *params,
		struct netlink_ext_ack *extack)
{
	struct tpmr_priv *tpmr = netdev_priv(dev);

	tpmr->dev = dev;
	tpmr->n_ports = 0;

	return register_netdevice(dev);
}

static const struct nla_policy tpmr_policy[IFLA_TPMR_MAX + 1] = {
	/* reserved for future per-instance flags */
};

static int __init tpmr_module_init(void)
{
	int err;

	err = register_netdevice_notifier(&tpmr_notifier_block);
	if (err)
		return err;

	err = rtnl_link_register(&tpmr_link_ops);
	if (err)
		unregister_netdevice_notifier(&tpmr_notifier_block);
	return err;
}

static void __exit tpmr_module_exit(void)
{
	rtnl_link_unregister(&tpmr_link_ops);
	unregister_netdevice_notifier(&tpmr_notifier_block);
}

static int tpmr_dev_init(struct net_device *dev)
{
	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;
	return 0;
}

static void tpmr_dev_uninit(struct net_device *dev)
{
	free_percpu(dev->tstats);
}

static void tpmr_dellink(struct net_device *dev, struct list_head *head)
{
	struct tpmr_priv *tpmr = netdev_priv(dev);
	unsigned int slot;

	for (slot = 0; slot < TPMR_MAX_PORTS; slot++) {
		struct net_device *slave = rtnl_dereference(tpmr->ports[slot].dev);

		if (slave)
			tpmr_del_slave(dev, slave);
	}
	unregister_netdevice_queue(dev, head);
}

static struct rtnl_link_ops tpmr_link_ops __read_mostly = {
	.kind           = TPMR_DRV_NAME,
	.priv_size      = sizeof(struct tpmr_priv),
	.setup          = tpmr_setup,
	.newlink        = tpmr_newlink,
	.dellink        = tpmr_dellink,
	.policy         = tpmr_policy,
	.maxtype        = IFLA_TPMR_MAX,
};

static const struct net_device_ops tpmr_netdev_ops = {
	.ndo_init               = tpmr_dev_init,
	.ndo_uninit             = tpmr_dev_uninit,
	.ndo_start_xmit         = tpmr_start_xmit,
	.ndo_get_stats64        = dev_get_tstats64,
	.ndo_add_slave          = tpmr_add_slave,
	.ndo_del_slave          = tpmr_del_slave,
	.ndo_set_mac_address    = eth_mac_addr,
	.ndo_validate_addr      = eth_validate_addr,
};

module_init(tpmr_module_init);
module_exit(tpmr_module_exit);

MODULE_AUTHOR("David Carlier <devnexen@gmail.com>");
MODULE_DESCRIPTION("IEEE 802.1Q Two-Port MAC Relay (TPMR) driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS_RTNL_LINK(TPMR_DRV_NAME);

