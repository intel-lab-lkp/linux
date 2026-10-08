// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/etherdevice.h>
#include <linux/if_link.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/rtnetlink.h>
#include <net/pon.h>

#include "pon.h"

struct pon_gem_priv {
	struct pon_dev *pdev;
	/* Fixed for the life of the interface: a GEM port that carries one
	 * cannot be removed, so the transmit path reads it without the lock.
	 */
	u16 gem_id;
};

/* A policy carries its bounds in an s16, so anything above 32767 needs the
 * full range form. NLA_POLICY_MAX() would store a negative maximum and warn
 * on every message.
 */
static const struct netlink_range_validation pon_gem_id_range = {
	.min = PON_GEM_PORT_ID_ASSIGNABLE_MIN,
	.max = PON_GEM_PORT_ID_ASSIGNABLE_MAX,
};

static const struct nla_policy pon_gem_link_policy[IFLA_GEM_MAX + 1] = {
	[IFLA_GEM_ID] = NLA_POLICY_FULL_RANGE(NLA_U32, &pon_gem_id_range),
};

/**
 * pon_gem_xmit() - send one frame on the GEM port of a GEM interface
 * @skb:	the frame
 * @dev:	the GEM network device
 *
 * Implements ndo_start_xmit. The driver's gem_xmit callback stamps the GEM
 * port into the descriptor. Without that callback, or once the driver has
 * unregistered, the frame is dropped and counted. A frame the callback
 * refuses is counted as dropped too: the link is down, the GEM port has no
 * channel or the ring is full, which the data interface counts the same way.
 *
 * Return: NETDEV_TX_OK, always.
 */
static netdev_tx_t pon_gem_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct pon_gem_priv *priv = netdev_priv(dev);
	struct pon_dev *pdev = priv->pdev;
	const struct pon_dev_ops *ops;
	unsigned int len = skb->len;
	int err;

	/* A driver that cannot stamp a GEM port into its descriptor gives the
	 * interface a name, a place in the link hierarchy and counters and
	 * nothing else. A driver that has unregistered has no ops at all.
	 */
	rcu_read_lock();
	ops = READ_ONCE(pdev->ops);
	if (!ops || !ops->gem_xmit) {
		rcu_read_unlock();
		DEV_STATS_INC(dev, tx_dropped);
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	err = ops->gem_xmit(pdev, priv->gem_id, skb);
	rcu_read_unlock();

	if (err) {
		DEV_STATS_INC(dev, tx_dropped);
		return NETDEV_TX_OK;
	}

	dev_sw_netstats_tx_add(dev, 1, len);
	return NETDEV_TX_OK;
}

/**
 * pon_gem_get_iflink() - the link of a GEM interface
 * @dev:	the GEM network device
 *
 * Implements ndo_get_iflink. A GEM interface is created on the PON data
 * interface, so a dump names that one as its link.
 *
 * Return: the ifindex of the data interface, or 0 before the interface is
 * attached.
 */
static int pon_gem_get_iflink(const struct net_device *dev)
{
	const struct pon_gem_priv *priv = netdev_priv(dev);

	if (!priv->pdev)
		return 0;

	return READ_ONCE(priv->pdev->main_netdev->ifindex);
}

/**
 * pon_gem_change_mtu() - change the MTU of a GEM interface
 * @dev:	the GEM network device
 * @mtu:	the new MTU
 *
 * Implements ndo_change_mtu. The conduit carries the frames of every PON
 * network device, so it must take the new MTU first.
 *
 * Context: Called with rtnl held.
 * Return: 0, or what pon_conduit_mtu_set() answered.
 */
static int pon_gem_change_mtu(struct net_device *dev, int mtu)
{
	struct pon_gem_priv *priv = netdev_priv(dev);
	int err;

	if (priv->pdev) {
		err = pon_conduit_mtu_set(priv->pdev, dev, mtu);
		if (err)
			return err;
	}

	WRITE_ONCE(dev->mtu, mtu);

	return 0;
}

static const struct net_device_ops pon_gem_netdev_ops = {
	.ndo_start_xmit		= pon_gem_xmit,
	.ndo_change_mtu		= pon_gem_change_mtu,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_iflink		= pon_gem_get_iflink,
};

/**
 * pon_gem_netdev_id() - the GEM port a GEM network device names
 * @dev:	a network device
 * @gem_id:	filled in with the GEM port id on success
 *
 * The GEM port id is fixed for the life of the interface, so a caller that
 * only wants to know which GEM port a device names needs no lock.
 *
 * Return: 0, or -ENODEV when @dev is not a GEM network device.
 */
int pon_gem_netdev_id(const struct net_device *dev, u16 *gem_id)
{
	const struct pon_gem_priv *priv;

	if (dev->netdev_ops != &pon_gem_netdev_ops)
		return -ENODEV;

	priv = netdev_priv(dev);
	*gem_id = priv->gem_id;

	return 0;
}

/**
 * pon_gem_dev_destructor() - release what a GEM interface holds
 * @dev:	the GEM network device
 *
 * The priv_destructor of a GEM network device. Drops the reference on the
 * instance that pon_gem_newlink() took, if the interface was attached.
 */
static void pon_gem_dev_destructor(struct net_device *dev)
{
	struct pon_gem_priv *priv = netdev_priv(dev);

	if (priv->pdev)
		pon_dev_put(priv->pdev);
}

/**
 * pon_gem_dev_setup() - initialize a new GEM network device
 * @dev:	the GEM network device
 *
 * Implements the setup op of the "gem" rtnl_link_ops.
 */
static void pon_gem_dev_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &pon_gem_netdev_ops;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	dev->needs_free_netdev = true;
	dev->priv_destructor = pon_gem_dev_destructor;
	dev->priv_flags |= IFF_NO_QUEUE;
	dev->lltx = true;
	dev->netns_immutable = true;
	dev->max_mtu = ETH_MAX_MTU;
}

/**
 * pon_gem_link_target() - the GEM object a new GEM interface is created for
 * @pdev:	PON device structure
 * @dev:	the new GEM network device
 * @lower:	the lower device the request named
 * @gem_id:	the GEM port id the request named
 * @extack:	netlink extended ack for the reason of a refusal
 *
 * Context: Called with @pdev->lock held.
 * Return: the GEM object, or ERR_PTR() of -ENODEV when @pdev is not
 * registered, -EINVAL when @lower is not the data interface or lives in
 * another network namespace than @dev, -ENOENT when no GEM object has
 * @gem_id, or -EBUSY when the GEM port has a network device already.
 */
static struct pon_gem *pon_gem_link_target(struct pon_dev *pdev,
					   const struct net_device *dev,
					   const struct net_device *lower,
					   u16 gem_id,
					   struct netlink_ext_ack *extack)
{
	struct pon_gem *gem;

	lockdep_assert_held(&pdev->lock);

	if (!pon_dev_is_registered(pdev))
		return ERR_PTR(-ENODEV);

	/* The conduit carries the same pointer and is not a PON interface. */
	if (lower != pdev->main_netdev) {
		NL_SET_ERR_MSG(extack,
			       "lower device is not the PON data interface");
		return ERR_PTR(-EINVAL);
	}
	if (!net_eq(dev_net(dev), dev_net(lower))) {
		NL_SET_ERR_MSG(extack,
			       "a GEM device lives in its PON device's namespace");
		return ERR_PTR(-EINVAL);
	}

	gem = pon_gem_find(pdev, gem_id);
	if (!gem) {
		NL_SET_ERR_MSG(extack, "no GEM object with this id");
		return ERR_PTR(-ENOENT);
	}
	if (gem->gem_netdev) {
		NL_SET_ERR_MSG(extack,
			       "the GEM port already has a network device");
		return ERR_PTR(-EBUSY);
	}

	return gem;
}

/**
 * pon_gem_link_attach() - attach a registered GEM interface to its GEM port
 * @pdev:	PON device structure
 * @dev:	the GEM network device, registered
 * @lower:	the lower device the request named
 * @gem_id:	the GEM port id the request named
 * @extack:	netlink extended ack for the reason of a refusal
 *
 * Checks the target again, because the instance lock was dropped while
 * @dev was registered. Then it links @dev and the GEM object and lets the
 * receive path find @dev.
 *
 * Context: Called with rtnl and @pdev->lock held.
 * Return: 0, or a negative errno from pon_gem_link_target() or the xarray.
 */
static int pon_gem_link_attach(struct pon_dev *pdev, struct net_device *dev,
			       const struct net_device *lower, u16 gem_id,
			       struct netlink_ext_ack *extack)
{
	struct pon_gem_priv *priv = netdev_priv(dev);
	struct pon_gem *gem;
	int err;

	lockdep_assert_held(&pdev->lock);

	gem = pon_gem_link_target(pdev, dev, lower, gem_id, extack);
	if (IS_ERR(gem))
		return PTR_ERR(gem);

	err = xa_err(xa_store(&pdev->gem_netdevs, gem_id, dev, GFP_KERNEL));
	if (err)
		return err;

	priv->pdev = pdev;
	gem->gem_netdev = dev;
	rcu_assign_pointer(dev->pon_dev, pdev);
	pon_dev_carrier_update(pdev);

	return 0;
}

/**
 * pon_gem_newlink() - create a GEM interface
 * @dev:	the new GEM network device
 * @params:	the attributes of the request
 * @extack:	netlink extended ack for the reason of a refusal
 *
 * Implements the newlink op of the "gem" rtnl_link_ops. The request names
 * the PON data interface as its link and a GEM port id, which the policy
 * limits to the assignable XGEM Port-IDs of ITU-T G.9807.1 Table C.6.6.
 * The interface takes its MTU range from the data interface. On success
 * it holds a reference on the instance until pon_gem_dev_destructor().
 *
 * Context: Called with rtnl held.
 * Return: 0, -EINVAL for a missing link, a missing GEM port id or an MTU
 * out of range, -ENODEV when the link does not exist, -EOPNOTSUPP when the
 * link is not a PON device, or a negative errno.
 */
static int pon_gem_newlink(struct net_device *dev,
			   struct rtnl_newlink_params *params,
			   struct netlink_ext_ack *extack)
{
	struct nlattr **data = params->data;
	struct nlattr **tb = params->tb;
	struct pon_gem_priv *priv;
	struct net_device *lower;
	struct pon_dev *pdev;
	struct pon_gem *gem;
	u16 gem_id;
	int err;

	if (!tb[IFLA_LINK]) {
		NL_SET_ERR_MSG(extack, "a lower PON device is required");
		return -EINVAL;
	}
	if (!data || !data[IFLA_GEM_ID]) {
		NL_SET_ERR_MSG(extack, "the GEM port id is required");
		return -EINVAL;
	}
	gem_id = nla_get_u32(data[IFLA_GEM_ID]);

	lower = __dev_get_by_index(rtnl_newlink_link_net(params),
				   nla_get_u32(tb[IFLA_LINK]));
	if (!lower)
		return -ENODEV;

	rcu_read_lock();
	pdev = rcu_dereference(lower->pon_dev);
	if (pdev && !pon_dev_tryget(pdev))
		pdev = NULL;
	rcu_read_unlock();

	if (!pdev) {
		NL_SET_ERR_MSG(extack, "lower device is not a PON device");
		return -EOPNOTSUPP;
	}

	mutex_lock(&pdev->lock);
	gem = pon_gem_link_target(pdev, dev, lower, gem_id, extack);
	mutex_unlock(&pdev->lock);
	if (IS_ERR(gem)) {
		err = PTR_ERR(gem);
		goto err_put;
	}

	if (!tb[IFLA_MTU]) {
		dev->mtu = lower->mtu;
	} else if (dev->mtu < lower->min_mtu || dev->mtu > lower->max_mtu) {
		NL_SET_ERR_MSG(extack,
			       "the MTU is outside the range of the PON data interface");
		err = -EINVAL;
		goto err_put;
	}
	dev->min_mtu = lower->min_mtu;
	dev->max_mtu = lower->max_mtu;

	err = pon_conduit_mtu_set(pdev, dev, dev->mtu);
	if (err) {
		NL_SET_ERR_MSG(extack, "the conduit refused the MTU");
		goto err_put;
	}

	if (!tb[IFLA_ADDRESS])
		eth_hw_addr_inherit(dev, lower);

	SET_NETDEV_DEV(dev, pdev->parent);
	priv = netdev_priv(dev);
	priv->gem_id = gem_id;

	err = register_netdevice(dev);
	if (err)
		goto err_put;

	mutex_lock(&pdev->lock);
	err = pon_gem_link_attach(pdev, dev, lower, gem_id, extack);
	mutex_unlock(&pdev->lock);
	if (err) {
		unregister_netdevice(dev);
		goto err_put;
	}

	return 0;

err_put:
	pon_dev_put(pdev);
	return err;
}

/**
 * pon_gem_link_detach() - detach a GEM interface from its GEM port
 * @pdev:	PON device structure
 * @dev:	the GEM network device
 *
 * Unlinks @dev and the GEM object and stops the receive path from finding
 * @dev. Does nothing for an interface that is detached already.
 *
 * Context: Called with rtnl and @pdev->lock held.
 */
static void pon_gem_link_detach(struct pon_dev *pdev, struct net_device *dev)
{
	struct pon_gem_priv *priv = netdev_priv(dev);
	struct pon_gem *gem;

	lockdep_assert_held(&pdev->lock);

	if (xa_load(&pdev->gem_netdevs, priv->gem_id) != dev)
		return;

	rcu_assign_pointer(dev->pon_dev, NULL);
	xa_erase(&pdev->gem_netdevs, priv->gem_id);
	gem = pon_gem_find(pdev, priv->gem_id);
	if (gem && gem->gem_netdev == dev)
		gem->gem_netdev = NULL;
}

/**
 * pon_gem_dellink() - delete a GEM interface
 * @dev:	the GEM network device
 * @head:	the list to queue @dev on for unregistration
 *
 * Implements the dellink op of the "gem" rtnl_link_ops. Detaches @dev from
 * its GEM port (if it is still attached) and queues it for unregistration.
 *
 * Context: Called with rtnl held.
 */
static void pon_gem_dellink(struct net_device *dev, struct list_head *head)
{
	struct pon_gem_priv *priv = netdev_priv(dev);
	struct pon_dev *pdev = priv->pdev;

	if (pdev) {
		mutex_lock(&pdev->lock);
		pon_gem_link_detach(pdev, dev);
		mutex_unlock(&pdev->lock);
	}

	unregister_netdevice_queue(dev, head);
}

/**
 * pon_gem_get_size() - the size of the link info of a GEM interface
 * @dev:	the GEM network device
 *
 * Implements the get_size op of the "gem" rtnl_link_ops.
 *
 * Return: the room pon_gem_fill_info() needs.
 */
static size_t pon_gem_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(u32));
}

/**
 * pon_gem_fill_info() - put the link info of a GEM interface
 * @skb:	the netlink message
 * @dev:	the GEM network device
 *
 * Implements the fill_info op of the "gem" rtnl_link_ops. Puts the GEM
 * port id as IFLA_GEM_ID.
 *
 * Return: 0, or -EMSGSIZE when @skb has no room.
 */
static int pon_gem_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct pon_gem_priv *priv = netdev_priv(dev);

	if (nla_put_u32(skb, IFLA_GEM_ID, priv->gem_id))
		return -EMSGSIZE;

	return 0;
}

static struct rtnl_link_ops pon_gem_link_ops = {
	.kind		= PON_GEM_KIND,
	.priv_size	= sizeof(struct pon_gem_priv),
	.setup		= pon_gem_dev_setup,
	.maxtype	= IFLA_GEM_MAX,
	.policy		= pon_gem_link_policy,
	.newlink	= pon_gem_newlink,
	.dellink	= pon_gem_dellink,
	.get_size	= pon_gem_get_size,
	.fill_info	= pon_gem_fill_info,
};
MODULE_ALIAS_RTNL_LINK(PON_GEM_KIND);

/**
 * pon_gem_netdevs_unregister() - drop every GEM network device of a device
 * @pdev:	PON device structure
 *
 * Called on the unregister path without rtnl or the instance lock held. It
 * takes rtnl and then the instance lock, detaches every GEM network device
 * and unregisters them all before it drops rtnl. pon_gem_dellink() runs
 * under rtnl too, so each device is detached and unregistered exactly once,
 * by whichever of the two takes rtnl first.
 */
void pon_gem_netdevs_unregister(struct pon_dev *pdev)
{
	struct net_device *ndev;
	struct pon_gem *gem;
	LIST_HEAD(list_kill);

	rtnl_lock();
	mutex_lock(&pdev->lock);
	list_for_each_entry(gem, &pdev->gems, list) {
		ndev = gem->gem_netdev;
		if (!ndev)
			continue;

		pon_gem_link_detach(pdev, ndev);
		unregister_netdevice_queue(ndev, &list_kill);
	}
	mutex_unlock(&pdev->lock);

	unregister_netdevice_many(&list_kill);
	rtnl_unlock();
}

/**
 * pon_gem_link_register() - register the "gem" link type
 *
 * Return: 0, or the negative errno of rtnl_link_register().
 */
int pon_gem_link_register(void)
{
	return rtnl_link_register(&pon_gem_link_ops);
}

/**
 * pon_gem_link_unregister() - unregister the "gem" link type
 */
void pon_gem_link_unregister(void)
{
	rtnl_link_unregister(&pon_gem_link_ops);
}
