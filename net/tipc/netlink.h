/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/netlink.h: Include file for TIPC netlink code
 *
 * Copyright (c) 2014, Ericsson AB
 * All rights reserved.
 */

#ifndef _TIPC_NETLINK_H
#define _TIPC_NETLINK_H
#include <net/netlink.h>

extern struct genl_family tipc_genl_family;

struct tipc_nl_msg {
	struct sk_buff *skb;
	u32 portid;
	u32 seq;
};

extern const struct nla_policy tipc_nl_name_table_policy[];
extern const struct nla_policy tipc_nl_sock_policy[];
extern const struct nla_policy tipc_nl_net_policy[];
extern const struct nla_policy tipc_nl_link_policy[];
extern const struct nla_policy tipc_nl_node_policy[];
extern const struct nla_policy tipc_nl_prop_policy[];
extern const struct nla_policy tipc_nl_bearer_policy[];
extern const struct nla_policy tipc_nl_media_policy[];
extern const struct nla_policy tipc_nl_udp_policy[];
extern const struct nla_policy tipc_nl_monitor_policy[];

int tipc_netlink_start(void);
int tipc_netlink_compat_start(void);
void tipc_netlink_stop(void);
void tipc_netlink_compat_stop(void);

#endif
