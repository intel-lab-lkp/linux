/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#ifndef __NET_PON_FUNCTIONS_H
#define __NET_PON_FUNCTIONS_H

#include <linux/netdevice.h>
#include <linux/rcupdate.h>
#include <net/pon/types.h>

struct pon_dev;
struct pon_dev_caps;
struct pon_dev_ops;
struct sk_buff;

#if IS_ENABLED(CONFIG_PON)

void pon_work_queue(struct pon_dev *pdev, struct pon_work *work);

/**
 * pon_work_init() - prepare a work item
 * @work: the item
 * @func: the handler pon_work_queue() runs
 *
 * Call once before the item is first queued.
 */
static inline void pon_work_init(struct pon_work *work, pon_work_func_t func)
{
	INIT_LIST_HEAD(&work->entry);
	work->func = func;
}

struct pon_dev *pon_dev_create(struct net_device *netdev,
			       struct device *parent,
			       const struct pon_dev_ops *ops,
			       const struct pon_dev_caps *caps,
			       enum pon_mode mode, void *priv_ptr);
void pon_dev_unregister(struct pon_dev *pdev);
void pon_dev_put(struct pon_dev *pdev);

int pon_dev_state_report(struct pon_dev *pdev,
			 enum pon_ploam_state state);
__printf(2, 3) void pon_dev_log(struct pon_dev *pdev, const char *fmt, ...);
void pon_dev_event(struct pon_dev *pdev, const struct pon_event *ev);

int pon_conduit_register(struct net_device *conduit,
			 const struct pon_conduit_ops *ops);
void pon_conduit_unregister(struct net_device *conduit);
int pon_conduit_rx(struct net_device *conduit, struct sk_buff *skb,
		   const struct pon_rx_info *info);
int pon_conduit_xmit(struct pon_dev *pdev, struct sk_buff *skb,
		     const struct pon_tx_info *info);
int pon_conduit_addr_set(struct pon_dev *pdev, const u8 *addr);
int pon_conduit_mtu_set(struct pon_dev *pdev, const struct net_device *dev,
			unsigned int mtu);
/**
 * netdev_uses_pon() - whether a network device belongs to a PON MAC
 * @dev: the network device
 *
 * True for the PON data interface, for a GEM network device and for a
 * conduit that is paired with a PON MAC.
 *
 * Return: true when @dev points at a PON instance.
 */
static inline bool netdev_uses_pon(const struct net_device *dev)
{
	return rcu_access_pointer(dev->pon_dev);
}

#else /* CONFIG_PON */

/**
 * pon_conduit_register() - offer a network device's rings to a PON MAC
 * @conduit: the ethernet device
 * @ops: what the driver does for the MAC
 *
 * The stub for a kernel without CONFIG_PON.
 *
 * Return: -ENOENT, there is no PON MAC to serve.
 */
static inline int pon_conduit_register(struct net_device *conduit,
				       const struct pon_conduit_ops *ops)
{
	return -ENOENT;
}

/**
 * pon_conduit_unregister() - take a network device's rings back
 * @conduit: the ethernet device
 *
 * The stub for a kernel without CONFIG_PON. It does nothing.
 */
static inline void pon_conduit_unregister(struct net_device *conduit)
{
}

/**
 * pon_conduit_rx() - hand one received frame to the PON MAC that owns it
 * @conduit: the ethernet device the frame arrived on
 * @skb: the frame
 * @info: what the receive descriptor said about it
 *
 * The stub for a kernel without CONFIG_PON. The frame stays the conduit's.
 *
 * Return: -ENODEV.
 */
static inline int pon_conduit_rx(struct net_device *conduit,
				 struct sk_buff *skb,
				 const struct pon_rx_info *info)
{
	return -ENODEV;
}

/**
 * netdev_uses_pon() - whether a network device belongs to a PON MAC
 * @dev: the network device
 *
 * The stub for a kernel without CONFIG_PON.
 *
 * Return: false.
 */
static inline bool netdev_uses_pon(const struct net_device *dev)
{
	return false;
}

#endif /* CONFIG_PON */

#endif /* __NET_PON_FUNCTIONS_H */
