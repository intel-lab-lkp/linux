// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 *		James Yonan <james@openvpn.net>
 */

#include "main.h"
#include "io.h"

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/version.h>


/* Driver info */
#define DRV_NAME	"ovpn"
#define DRV_VERSION	OVPN_VERSION
#define DRV_DESCRIPTION	"OpenVPN data channel offload (ovpn)"
#define DRV_COPYRIGHT	"(C) 2020-2024 OpenVPN, Inc."

/* Net device open */
static int ovpn_net_open(struct net_device *dev)
{
	struct in_device *dev_v4 = __in_dev_get_rtnl(dev);

	if (dev_v4) {
		/* disable redirects as Linux gets confused by ovpn handling same-LAN routing */
		IN_DEV_CONF_SET(dev_v4, SEND_REDIRECTS, false);
		IPV4_DEVCONF_ALL(dev_net(dev), SEND_REDIRECTS) = false;
	}

	netif_tx_start_all_queues(dev);
	return 0;
}

/* Net device stop -- called prior to device unload */
static int ovpn_net_stop(struct net_device *dev)
{
	netif_tx_stop_all_queues(dev);
	return 0;
}

bool ovpn_dev_is_valid(const struct net_device *dev)
{
	return dev->netdev_ops->ndo_start_xmit == ovpn_net_xmit;
}

static const struct net_device_ops ovpn_netdev_ops = {
	.ndo_open		= ovpn_net_open,
	.ndo_stop		= ovpn_net_stop,
	.ndo_start_xmit		= ovpn_net_xmit,
	.ndo_get_stats64        = dev_get_tstats64,
};

static int ovpn_netdev_notifier_call(struct notifier_block *nb,
				     unsigned long state, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (!ovpn_dev_is_valid(dev))
		return NOTIFY_DONE;

	switch (state) {
	case NETDEV_REGISTER:
		/* add device to internal list for later destruction upon unregistration */
		break;
	case NETDEV_UNREGISTER:
		/* can be delivered multiple times, so check registered flag, then
		 * destroy the interface
		 */
		break;
	case NETDEV_POST_INIT:
	case NETDEV_GOING_DOWN:
	case NETDEV_DOWN:
	case NETDEV_UP:
	case NETDEV_PRE_UP:
	default:
		return NOTIFY_DONE;
	}

	return NOTIFY_OK;
}

static struct notifier_block ovpn_netdev_notifier = {
	.notifier_call = ovpn_netdev_notifier_call,
};

static int __init ovpn_init(void)
{
	int err = register_netdevice_notifier(&ovpn_netdev_notifier);

	if (err) {
		pr_err("ovpn: can't register netdevice notifier: %d\n", err);
		return err;
	}

	return 0;
}

static __exit void ovpn_cleanup(void)
{
	unregister_netdevice_notifier(&ovpn_netdev_notifier);
}

module_init(ovpn_init);
module_exit(ovpn_cleanup);

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_COPYRIGHT);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
