// SPDX-License-Identifier: GPL-2.0
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#include "io.h"
#include "ovpnstruct.h"
#include "netlink.h"
#include "peer.h"

#include <linux/netdevice.h>
#include <linux/skbuff.h>


int ovpn_struct_init(struct net_device *dev)
{
	struct ovpn_struct *ovpn = netdev_priv(dev);
	int err;

	memset(ovpn, 0, sizeof(*ovpn));

	ovpn->dev = dev;

	err = ovpn_nl_init(ovpn);
	if (err < 0)
		return err;

	spin_lock_init(&ovpn->lock);

	ovpn->events_wq = alloc_workqueue("ovpn-events-wq-%s", WQ_MEM_RECLAIM, 0, dev->name);
	if (!ovpn->events_wq)
		return -ENOMEM;

	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;

	err = security_tun_dev_alloc_security(&ovpn->security);
	if (err < 0)
		return err;

	return 0;
}

/* Send user data to the network
 */
netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	skb_tx_error(skb);
	kfree_skb(skb);
	return NET_XMIT_DROP;
}
