/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/name_distr.h: Include file for TIPC name distribution code
 *
 * Copyright (c) 2000-2006, Ericsson AB
 * Copyright (c) 2005, Wind River Systems
 * All rights reserved.
 */

#ifndef _TIPC_NAME_DISTR_H
#define _TIPC_NAME_DISTR_H

#include "name_table.h"

#define ITEM_SIZE sizeof(struct distr_item)

/**
 * struct distr_item - publication info distributed to other nodes
 * @type: name sequence type
 * @lower: name sequence lower bound
 * @upper: name sequence upper bound
 * @port: publishing port reference
 * @key: publication key
 *
 * ===> All fields are stored in network byte order. <===
 *
 * First 3 fields identify (name or) name sequence being published.
 * Reference field uniquely identifies port that published name sequence.
 * Key field uniquely identifies publication, in the event a port has
 * multiple publications of the same name sequence.
 *
 * Note: There is no field that identifies the publishing node because it is
 * the same for all items contained within a publication message.
 */
struct distr_item {
	__be32 type;
	__be32 lower;
	__be32 upper;
	__be32 port;
	__be32 key;
};

struct sk_buff *tipc_named_publish(struct net *net, struct publication *publ);
struct sk_buff *tipc_named_withdraw(struct net *net, struct publication *publ);
void tipc_named_node_up(struct net *net, u32 dnode, u16 capabilities);
void tipc_named_rcv(struct net *net, struct sk_buff_head *namedq,
		    u16 *rcv_nxt, bool *open);
void tipc_named_reinit(struct net *net);
void tipc_publ_notify(struct net *net, struct list_head *nsub_list,
		      u32 addr, u16 capabilities);

#endif
