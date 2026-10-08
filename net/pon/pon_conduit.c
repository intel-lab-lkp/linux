// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/property.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <net/pon.h>

#include "pon.h"

/* One registered conduit. It stays on the list whether or not a PON MAC has
 * claimed it, so that a MAC which registers later, or registers again, finds
 * it.
 */
struct pon_conduit {
	struct list_head list;
	struct net_device *netdev;
	struct fwnode_handle *mac;
	const struct pon_conduit_ops *ops;
	struct pon_dev *pdev;
	bool up;
};

/* guarded by pon_devs_lock */
static LIST_HEAD(pon_conduits);

/**
 * pon_conduit_find() - look up the conduit of a network device
 * @netdev:	the ethernet device a driver registered as a conduit
 *
 * Context: Called with pon_devs_lock held.
 * Return: the conduit, or NULL when @netdev is not a registered conduit.
 */
static struct pon_conduit *pon_conduit_find(const struct net_device *netdev)
{
	struct pon_conduit *entry;

	lockdep_assert_held(&pon_devs_lock);
	list_for_each_entry(entry, &pon_conduits, list)
		if (entry->netdev == netdev)
			return entry;
	return NULL;
}

/**
 * pon_conduit_find_free() - look up an unclaimed conduit that names a MAC
 * @pdev:	PON device structure
 *
 * A conduit names its MAC through the firmware node of @pdev's parent.
 *
 * Context: Called with pon_devs_lock held.
 * Return: a conduit that names the MAC of @pdev and that no instance has
 * claimed, or NULL when there is none.
 */
static struct pon_conduit *pon_conduit_find_free(const struct pon_dev *pdev)
{
	struct fwnode_handle *mac = dev_fwnode(pdev->parent);
	struct pon_conduit *entry;

	lockdep_assert_held(&pon_devs_lock);
	list_for_each_entry(entry, &pon_conduits, list)
		if (entry->mac == mac && !entry->pdev)
			return entry;
	return NULL;
}

/**
 * pon_conduit_pair() - pair a conduit with an instance
 * @entry:	the conduit
 * @pdev:	PON device structure
 *
 * Takes a reference on the conduit's network device and publishes the
 * pointer from each side to the other for the receive and transmit paths.
 *
 * Context: Called with pon_devs_lock held.
 */
static void pon_conduit_pair(struct pon_conduit *entry, struct pon_dev *pdev)
{
	lockdep_assert_held(&pon_devs_lock);

	entry->pdev = pdev;
	netdev_hold(entry->netdev, &pdev->conduit_tracker, GFP_KERNEL);
	rcu_assign_pointer(pdev->conduit, entry);
	rcu_assign_pointer(entry->netdev->pon_dev, pdev);
}

/**
 * pon_conduit_unpair() - break the pairing of a conduit and its instance
 * @entry:	the conduit, paired with an instance
 *
 * The receive path finds the instance through the conduit and the transmit
 * path finds the conduit through the instance, both under RCU, so both
 * pointers go before the grace period and the reference after it.
 *
 * Context: Called with pon_devs_lock held. Sleeps.
 */
static void pon_conduit_unpair(struct pon_conduit *entry)
{
	struct pon_dev *pdev = entry->pdev;

	lockdep_assert_held(&pon_devs_lock);

	rcu_assign_pointer(entry->netdev->pon_dev, NULL);
	rcu_assign_pointer(pdev->conduit, NULL);
	synchronize_net();
	netdev_put(entry->netdev, &pdev->conduit_tracker);
	entry->pdev = NULL;
}

/**
 * pon_conduit_hold() - hold the conduit across a call that may sleep
 * @pdev:	PON device structure
 * @ops:	where to store what the conduit's driver does, or NULL
 * @tracker:	the tracker of the reference
 *
 * Return: the conduit's ethernet device with a reference held, or NULL when
 * no driver has claimed the instance.
 */
struct net_device *pon_conduit_hold(struct pon_dev *pdev,
				    const struct pon_conduit_ops **ops,
				    netdevice_tracker *tracker)
{
	struct net_device *conduit = NULL;
	struct pon_conduit *entry;

	rcu_read_lock();
	entry = rcu_dereference(pdev->conduit);
	if (entry) {
		conduit = entry->netdev;
		if (ops)
			*ops = entry->ops;
		netdev_hold(conduit, tracker, GFP_ATOMIC);
	}
	rcu_read_unlock();

	return conduit;
}

/**
 * pon_conduit_running() - whether the instance has a conduit that is up
 * @pdev:	PON device structure
 *
 * A conduit counts as up from NETDEV_UP until NETDEV_GOING_DOWN, so the PON
 * network devices lose their carrier before the conduit closes.
 *
 * Context: Any context.
 * Return: true when a conduit is paired with @pdev and is up.
 */
bool pon_conduit_running(struct pon_dev *pdev)
{
	struct pon_conduit *entry;
	bool up;

	rcu_read_lock();
	entry = rcu_dereference(pdev->conduit);
	up = entry && READ_ONCE(entry->up);
	rcu_read_unlock();

	return up;
}

/**
 * pon_conduit_carrier_update() - set the carrier after a conduit change
 * @pdev:	PON device structure
 *
 * Context: Takes @pdev->lock. Called without it.
 */
static void pon_conduit_carrier_update(struct pon_dev *pdev)
{
	mutex_lock(&pdev->lock);
	if (pon_dev_is_registered(pdev))
		pon_dev_carrier_update(pdev);
	mutex_unlock(&pdev->lock);
}

/**
 * pon_conduit_addr_set() - give the conduit the PON data interface's address
 * @pdev:	PON device structure
 * @addr:	the address the PON network devices carry
 *
 * The frame engine behind a conduit recognizes the frames addressed to the
 * ONU by the conduit's own address, so the two are kept the same. Called with
 * rtnl held.
 *
 * Return: 0, or what the conduit's driver answered.
 */
int pon_conduit_addr_set(struct pon_dev *pdev, const u8 *addr)
{
	struct sockaddr_storage ss = {};
	struct net_device *conduit;
	netdevice_tracker tracker;
	int err = 0;

	ASSERT_RTNL();

	conduit = pon_conduit_hold(pdev, NULL, &tracker);
	if (!conduit)
		return 0;

	if (!ether_addr_equal(conduit->dev_addr, addr)) {
		ss.ss_family = conduit->type;
		memcpy(ss.__data, addr, ETH_ALEN);
		err = dev_set_mac_address(conduit, &ss, NULL);
	}

	netdev_put(conduit, &tracker);
	return err;
}
EXPORT_SYMBOL_GPL(pon_conduit_addr_set);

/**
 * pon_conduit_sync() - give a newly paired conduit the address and the MTU
 * @pdev:	PON device structure
 *
 * After a pairing, outside every lock: the conduit's driver programs its
 * address and its MTU under rtnl, which is never taken while an instance lock
 * is held. The carrier follows the new conduit. A conduit that refuses the
 * address or the MTU is warned about and stays paired.
 *
 * Does nothing once pon_dev_unregister() has begun. It tests that under rtnl,
 * which pon_dev_unregister() takes before it lets the driver free the data
 * network device, so the device read here is alive.
 *
 * Context: Takes rtnl and @pdev->lock. Called without rtnl, pon_devs_lock or
 * @pdev->lock held.
 */
void pon_conduit_sync(struct pon_dev *pdev)
{
	int err;

	rtnl_lock();
	if (READ_ONCE(pdev->going_away)) {
		rtnl_unlock();
		return;
	}
	err = pon_conduit_addr_set(pdev, pdev->main_netdev->dev_addr);
	if (err)
		netdev_warn(pdev->main_netdev,
			    "the conduit refused the address: %pe\n",
			    ERR_PTR(err));
	err = pon_conduit_mtu_set(pdev, NULL, 0);
	if (err)
		netdev_warn(pdev->main_netdev,
			    "the conduit refused the MTU: %pe\n", ERR_PTR(err));
	rtnl_unlock();

	pon_conduit_carrier_update(pdev);
}

/**
 * pon_conduit_register() - offer a network device's rings to a PON MAC
 * @netdev:	the registered ethernet device, whose firmware node names the
 *		MAC through a pon-handle reference
 * @ops:	what the driver does for the MAC
 *
 * Neither side waits for the other. A MAC that registers later, or again,
 * finds the conduit. Until then its frames are dropped and the conduit's own
 * receive path keeps what arrives.
 *
 * Context: Takes rtnl. Called without it.
 * Return: 0 once registered, -ENOENT when the firmware node names no PON MAC,
 * -EBUSY when the network device is a conduit already, or a negative errno.
 */
int pon_conduit_register(struct net_device *netdev,
			 const struct pon_conduit_ops *ops)
{
	struct pon_dev *pdev = NULL, *iter;
	struct fwnode_handle *mac;
	struct pon_conduit *entry;
	unsigned long id;

	mac = fwnode_find_reference(dev_fwnode(&netdev->dev), "pon-handle", 0);
	if (IS_ERR(mac))
		return PTR_ERR(mac);

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry) {
		fwnode_handle_put(mac);
		return -ENOMEM;
	}
	entry->netdev = netdev;
	entry->mac = mac;
	entry->ops = ops;

	rtnl_lock();
	mutex_lock(&pon_devs_lock);

	if (pon_conduit_find(netdev)) {
		mutex_unlock(&pon_devs_lock);
		rtnl_unlock();
		fwnode_handle_put(mac);
		kfree(entry);
		return -EBUSY;
	}
	entry->up = netif_running(netdev);
	list_add_tail(&entry->list, &pon_conduits);

	xa_for_each(&pon_devs, id, iter) {
		if (dev_fwnode(iter->parent) != mac ||
		    rcu_access_pointer(iter->conduit))
			continue;
		pon_conduit_pair(entry, iter);
		pon_dev_get(iter);
		pdev = iter;
		break;
	}

	mutex_unlock(&pon_devs_lock);
	rtnl_unlock();

	if (pdev) {
		pon_conduit_sync(pdev);
		pon_dev_put(pdev);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pon_conduit_register);

/**
 * pon_conduit_unregister() - take a network device's rings back
 * @netdev:	the device pon_conduit_register() was given
 *
 * Call before unregistering the device. A MAC the device served keeps
 * running without a conduit and its PON network devices lose their carrier.
 */
void pon_conduit_unregister(struct net_device *netdev)
{
	struct pon_conduit *entry;
	struct pon_dev *pdev;

	mutex_lock(&pon_devs_lock);
	entry = pon_conduit_find(netdev);
	if (entry) {
		pdev = entry->pdev;
		if (pdev) {
			pon_conduit_unpair(entry);
			pon_conduit_carrier_update(pdev);
		}
		list_del(&entry->list);
	}
	mutex_unlock(&pon_devs_lock);

	if (!entry)
		return;

	fwnode_handle_put(entry->mac);
	kfree(entry);
}
EXPORT_SYMBOL_GPL(pon_conduit_unregister);

/**
 * pon_conduit_netdev_event() - follow a conduit that goes up or down
 * @nb:		the notifier block
 * @event:	what happened to the network device
 * @ptr:	struct netdev_notifier_info of the network device
 *
 * A registered conduit counts as up from NETDEV_UP until NETDEV_GOING_DOWN.
 * The PON network devices of the instance it serves get their carrier
 * again, so they never carry traffic into a conduit that is down.
 *
 * Context: Called with rtnl held. Takes pon_devs_lock and the instance lock.
 * Return: NOTIFY_DONE.
 */
static int pon_conduit_netdev_event(struct notifier_block *nb,
				    unsigned long event, void *ptr)
{
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);
	struct pon_conduit *entry;

	if (event != NETDEV_UP && event != NETDEV_GOING_DOWN)
		return NOTIFY_DONE;

	mutex_lock(&pon_devs_lock);
	entry = pon_conduit_find(netdev);
	if (entry) {
		WRITE_ONCE(entry->up, event == NETDEV_UP);
		if (entry->pdev)
			pon_conduit_carrier_update(entry->pdev);
	}
	mutex_unlock(&pon_devs_lock);

	return NOTIFY_DONE;
}

static struct notifier_block pon_conduit_netdev_notifier = {
	.notifier_call = pon_conduit_netdev_event,
};

/**
 * pon_conduit_notifier_register() - watch the conduits go up and down
 *
 * Return: 0, or the errno of register_netdevice_notifier().
 */
int pon_conduit_notifier_register(void)
{
	return register_netdevice_notifier(&pon_conduit_netdev_notifier);
}

/**
 * pon_conduit_notifier_unregister() - stop the watch of the conduits
 */
void pon_conduit_notifier_unregister(void)
{
	unregister_netdevice_notifier(&pon_conduit_netdev_notifier);
}

/**
 * pon_conduit_attach() - claim a waiting conduit for a new instance
 * @pdev:	PON device structure
 *
 * A new instance claims the conduit that names its MAC, if one is waiting.
 *
 * Context: Called with pon_devs_lock held.
 * Return: true when a conduit was paired and there is an address to sync
 * with pon_conduit_sync() once the locks are dropped, false otherwise.
 */
bool pon_conduit_attach(struct pon_dev *pdev)
{
	struct pon_conduit *entry;

	entry = pon_conduit_find_free(pdev);
	if (!entry)
		return false;

	pon_conduit_pair(entry, pdev);
	return true;
}

/**
 * pon_conduit_detach() - release the conduit an instance has claimed
 * @pdev:	PON device structure
 *
 * The conduit stays registered and waits for the next instance of its MAC.
 * Once this returns, no frame of the conduit reaches @pdev.
 *
 * Context: Takes pon_devs_lock. Sleeps.
 */
void pon_conduit_detach(struct pon_dev *pdev)
{
	struct pon_conduit *entry;

	mutex_lock(&pon_devs_lock);
	entry = rcu_dereference_protected(pdev->conduit,
					  lockdep_is_held(&pon_devs_lock));
	if (entry)
		pon_conduit_unpair(entry);
	mutex_unlock(&pon_devs_lock);
}

/**
 * pon_conduit_mtu_largest() - the MTU the conduit needs
 * @pdev:	PON device structure
 * @dev:	the PON network device whose MTU changes, or NULL when none does
 * @mtu:	the MTU @dev changes to
 *
 * Takes @mtu for @dev and the current MTU of every other PON network device
 * of @pdev. The floor is the extended OMCI PDU with its MIC, 1980 bytes,
 * ITU-T G.988 clause 11.2.5.
 *
 * Return: the largest of these MTUs.
 */
static unsigned int pon_conduit_mtu_largest(struct pon_dev *pdev,
					    const struct net_device *dev,
					    unsigned int mtu)
{
	struct net_device *gem;
	unsigned long id;

	mtu = max(mtu, PON_OMCI_MAX_LEN + PON_OMCI_MIC_LEN);

	if (dev != pdev->main_netdev)
		mtu = max(mtu, READ_ONCE(pdev->main_netdev->mtu));

	xa_for_each(&pdev->gem_netdevs, id, gem)
		if (dev != gem)
			mtu = max(mtu, READ_ONCE(gem->mtu));

	return mtu;
}

/**
 * pon_conduit_mtu_set() - make room on the conduit for a PON network device
 * @pdev:	PON device structure
 * @dev:	the PON network device whose MTU changes, or NULL when none does
 * @mtu:	the MTU @dev changes to
 *
 * The frames of every PON network device and the OMCI PDUs ride the conduit,
 * so the conduit takes the largest MTU among them and never less than the
 * longest OMCI PDU with its integrity field. Called with rtnl held.
 *
 * Return: 0, or what the conduit's driver answered.
 */
int pon_conduit_mtu_set(struct pon_dev *pdev, const struct net_device *dev,
			unsigned int mtu)
{
	const struct pon_conduit_ops *ops;
	struct net_device *conduit;
	netdevice_tracker tracker;
	int err = 0;

	ASSERT_RTNL();

	conduit = pon_conduit_hold(pdev, &ops, &tracker);
	if (!conduit)
		return 0;

	mtu = pon_conduit_mtu_largest(pdev, dev, mtu);
	if (mtu != READ_ONCE(conduit->mtu))
		err = dev_set_mtu(conduit, mtu);

	netdev_put(conduit, &tracker);

	return err;
}
EXPORT_SYMBOL_GPL(pon_conduit_mtu_set);

/**
 * pon_conduit_xmit() - send one frame through the conduit
 * @pdev:	PON device structure
 * @skb:	the frame, consumed whatever this returns
 * @info:	what the conduit needs to build the descriptor
 *
 * Return: 0 once the conduit has the frame, -ENETDOWN without a conduit,
 * -EBUSY when its ring is full.
 */
int pon_conduit_xmit(struct pon_dev *pdev, struct sk_buff *skb,
		     const struct pon_tx_info *info)
{
	struct pon_conduit *entry;
	netdev_tx_t ret;

	rcu_read_lock();
	entry = rcu_dereference(pdev->conduit);
	if (!entry) {
		rcu_read_unlock();
		dev_kfree_skb_any(skb);
		return -ENETDOWN;
	}
	ret = entry->ops->xmit(entry->netdev, skb, info);
	rcu_read_unlock();

	if (ret == NETDEV_TX_BUSY) {
		dev_kfree_skb_any(skb);
		return -EBUSY;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(pon_conduit_xmit);

/**
 * pon_conduit_rx() - hand one received frame to the PON MAC that owns it
 * @conduit:	the ethernet device the frame arrived on
 * @skb:	the frame, starting at its first byte: the MAC header of an
 *		Ethernet frame, the transaction id of an OMCI PDU
 * @info:	what the receive descriptor said about it
 *
 * Runs in the conduit's NAPI context. An OMCI PDU goes to the socket that
 * owns the OMCI channel and is consumed here, a frame on a GEM port that
 * carries a network device of its own goes there and everything else goes
 * to the PON data interface. A frame for a network device that is down is
 * dropped and counted in its rx_dropped.
 *
 * Return: 1 when the frame belongs to a PON network device (now skb->dev)
 * and the conduit delivers it, 0 when it was consumed here, or -ENODEV when
 * no PON MAC owns the conduit and the frame stays the conduit's.
 */
int pon_conduit_rx(struct net_device *conduit, struct sk_buff *skb,
		   const struct pon_rx_info *info)
{
	struct net_device *dev;
	struct pon_dev *pdev;
	unsigned int len;
	int ret;

	rcu_read_lock();

	pdev = rcu_dereference(conduit->pon_dev);
	if (!pdev) {
		ret = -ENODEV;
		goto out;
	}

	if (info->oam) {
		ret = pon_omci_conduit_rx(pdev, skb, info->mic_unchecked);
		goto out;
	}

	dev = xa_load(&pdev->gem_netdevs, info->gem);
	if (!dev)
		dev = pdev->main_netdev;

	if (!(READ_ONCE(dev->flags) & IFF_UP)) {
		dev_core_stats_rx_dropped_inc(dev);
		kfree_skb(skb);
		ret = 0;
		goto out;
	}

	len = skb->len;
	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev);
	dev_sw_netstats_rx_add(dev, len);
	ret = 1;

out:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(pon_conduit_rx);
