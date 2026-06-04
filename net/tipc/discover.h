/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/discover.h
 *
 * Copyright (c) 2003-2006, Ericsson AB
 * Copyright (c) 2005, 2010-2011, Wind River Systems
 * All rights reserved.
 */

#ifndef _TIPC_DISCOVER_H
#define _TIPC_DISCOVER_H

struct tipc_discoverer;

int tipc_disc_create(struct net *net, struct tipc_bearer *b_ptr,
		     struct tipc_media_addr *dest, struct sk_buff **skb);
void tipc_disc_delete(struct tipc_discoverer *req);
void tipc_disc_reset(struct net *net, struct tipc_bearer *b_ptr);
void tipc_disc_add_dest(struct tipc_discoverer *req);
void tipc_disc_remove_dest(struct tipc_discoverer *req);
void tipc_disc_rcv(struct net *net, struct sk_buff *buf,
		   struct tipc_bearer *b_ptr);

#endif
