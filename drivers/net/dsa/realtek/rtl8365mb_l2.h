/* SPDX-License-Identifier: GPL-2.0 */
/* Forwarding and multicast database interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#ifndef _REALTEK_RTL8365MB_L2_H
#define _REALTEK_RTL8365MB_L2_H

#include <linux/if_ether.h>
#include <linux/types.h>

#include "realtek.h"

/* It's valid for all family but RTL8370B, which has 4160 */
#define RTL8365MB_LEARN_LIMIT_MAX	2112

struct rtl8365mb_l2_uc_key {
	u8 mac_addr[ETH_ALEN];
	union {
		u16 vid; /* IVL */
		u16 fid; /* SVL */
	};
	bool ivl;
	u16 efid;
};

struct rtl8365mb_l2_uc {
	struct rtl8365mb_l2_uc_key key;
	u8 port;
	u8 age;
	u8 priority;

	bool sa_block;
	bool da_block;
	bool auth;
	bool is_static;
	bool sa_pri;
	bool fwd_pri;
};

int rtl8365mb_l2_get_next_uc(struct realtek_priv *priv, u16 *addr, u16 port,
			     struct rtl8365mb_l2_uc *uc);
int rtl8365mb_l2_add_uc(struct realtek_priv *priv, u16 port,
			const unsigned char addr[static ETH_ALEN],
			u16 efid, u16 vid);
int rtl8365mb_l2_del_uc(struct realtek_priv *priv, u16 port,
			const unsigned char addr[static ETH_ALEN],
			u16 efid, u16 vid);
int rtl8365mb_l2_flush(struct realtek_priv *priv, int port, u16 vid);

int rtl8365mb_l2_add_mc(struct realtek_priv *priv, u16 port,
			const unsigned char mac_addr[static ETH_ALEN],
			u16 vid);
int rtl8365mb_l2_del_mc(struct realtek_priv *priv, u16 port,
			const unsigned char mac_addr[static ETH_ALEN],
			u16 vid);

#endif /* _REALTEK_RTL8365MB_L2_H */
