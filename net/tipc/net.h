/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/net.h: Include file for TIPC network routing code
 *
 * Copyright (c) 1995-2006, 2014, Ericsson AB
 * Copyright (c) 2005, 2010-2011, Wind River Systems
 * All rights reserved.
 */

#ifndef _TIPC_NET_H
#define _TIPC_NET_H

#include <net/genetlink.h>

extern const struct nla_policy tipc_nl_net_policy[];

int tipc_net_init(struct net *net, u8 *node_id, u32 addr);
void tipc_net_finalize_work(struct work_struct *work);
void tipc_net_stop(struct net *net);
int tipc_nl_net_dump(struct sk_buff *skb, struct netlink_callback *cb);
int tipc_nl_net_set(struct sk_buff *skb, struct genl_info *info);
int __tipc_nl_net_set(struct sk_buff *skb, struct genl_info *info);
int tipc_nl_net_addr_legacy_get(struct sk_buff *skb, struct genl_info *info);

#endif
