/* SPDX-License-Identifier: GPL-2.0-only */
/* OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_OVPN_H_
#define _NET_OVPN_OVPN_H_

#include <linux/netdevice.h>

struct sk_buff;
struct ovpn_peer;
struct ovpn_struct;

int ovpn_struct_init(struct net_device *dev);
netdev_tx_t ovpn_net_xmit(struct sk_buff *skb, struct net_device *dev);
int ovpn_napi_poll(struct napi_struct *napi, int budget);
void ovpn_keepalive_xmit(struct ovpn_peer *peer);

int ovpn_recv(struct ovpn_struct *ovpn, struct ovpn_peer *peer, struct sk_buff *skb);

void ovpn_encrypt_work(struct work_struct *work);
void ovpn_decrypt_work(struct work_struct *work);

#endif /* _NET_OVPN_OVPN_H_ */
