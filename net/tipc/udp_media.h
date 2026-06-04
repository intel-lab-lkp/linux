/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/udp_media.h: Include file for UDP bearer media
 *
 * Copyright (c) 1996-2006, 2013-2016, Ericsson AB
 * Copyright (c) 2005, 2010-2011, Wind River Systems
 * All rights reserved.
 */

#ifdef CONFIG_TIPC_MEDIA_UDP
#ifndef _TIPC_UDP_MEDIA_H
#define _TIPC_UDP_MEDIA_H

#include <linux/ip.h>
#include <linux/udp.h>

int tipc_udp_nl_bearer_add(struct tipc_bearer *b, struct nlattr *attr);
int tipc_udp_nl_add_bearer_data(struct tipc_nl_msg *msg, struct tipc_bearer *b);
int tipc_udp_nl_dump_remoteip(struct sk_buff *skb, struct netlink_callback *cb);

/* check if configured MTU is too low for tipc headers */
static inline bool tipc_udp_mtu_bad(u32 mtu)
{
	if (mtu >= (TIPC_MIN_BEARER_MTU + sizeof(struct iphdr) +
	    sizeof(struct udphdr)))
		return false;

	pr_warn("MTU too low for tipc bearer\n");
	return true;
}

#endif
#endif
