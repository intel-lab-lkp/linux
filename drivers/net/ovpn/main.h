/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2019-2024 OpenVPN, Inc.
 *
 *  Author:	James Yonan <james@openvpn.net>
 *		Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_MAIN_H_
#define _NET_OVPN_MAIN_H_

#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/printk.h>
#include <linux/udp.h>

#ifndef OVPN_VERSION
#define OVPN_VERSION "3.0.0"
#endif

struct net_device;
struct ovpn_struct;
enum ovpn_mode;

bool ovpn_dev_is_valid(const struct net_device *dev);
int ovpn_iface_create(const char *name, enum ovpn_mode mode, struct net *net);
void ovpn_iface_destruct(struct ovpn_struct *ovpn, bool unregister_device);

#define SKB_HEADER_LEN                                       \
	(max(sizeof(struct iphdr), sizeof(struct ipv6hdr)) + \
	 sizeof(struct udphdr) + NET_SKB_PAD)

#define OVPN_HEAD_ROOM ALIGN(16 + SKB_HEADER_LEN, 4)
#define OVPN_MAX_PADDING 16
#define OVPN_QUEUE_LEN 1024

#endif /* _NET_OVPN_MAIN_H_ */
