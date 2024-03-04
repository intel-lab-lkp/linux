/* SPDX-License-Identifier: GPL-2.0-only */
/*  OpenVPN data channel offload
 *
 *  Copyright (C) 2020-2024 OpenVPN, Inc.
 *
 *  Author:	Antonio Quartulli <antonio@openvpn.net>
 */

#ifndef _NET_OVPN_NETLINK_H_
#define _NET_OVPN_NETLINK_H_

struct ovpn_struct;

int ovpn_nl_init(struct ovpn_struct *ovpn);
int ovpn_nl_register(void);
void ovpn_nl_unregister(void);

#endif /* _NET_OVPN_NETLINK_H_ */
