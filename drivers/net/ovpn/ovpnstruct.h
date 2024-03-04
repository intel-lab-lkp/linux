/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_OVPNSTRUCT_H_
#define _NET_OVPN_OVPNSTRUCT_H_

#include <uapi/linux/ovpn.h>
#include <linux/netdevice.h>
#include <linux/types.h>

/* Our state per ovpn interface */
struct ovpn_struct {
	struct net_device *dev;

	/* whether this device is still registered with netdev or not */
	bool registered;

	/* device operation mode (i.e. P2P, MP) */
	enum ovpn_mode mode;

	unsigned int max_tun_queue_len;

	netdev_features_t set_features;

	void *security;

	struct list_head dev_list;
};

#endif /* _NET_OVPN_OVPNSTRUCT_H_ */
