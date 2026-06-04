/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/group.h: Include file for TIPC group unicast/multicast functions
 *
 * Copyright (c) 2017, Ericsson AB
 * Copyright (c) 2020, Red Hat Inc
 * All rights reserved.
 */

#ifndef _TIPC_GROUP_H
#define _TIPC_GROUP_H

#include "core.h"

struct tipc_group;
struct tipc_member;
struct tipc_msg;

struct tipc_group *tipc_group_create(struct net *net, u32 portid,
				     struct tipc_group_req *mreq,
				     bool *group_is_open);
void tipc_group_join(struct net *net, struct tipc_group *grp, int *sk_rcv_buf);
void tipc_group_delete(struct net *net, struct tipc_group *grp);
void tipc_group_add_member(struct tipc_group *grp, u32 node,
			   u32 port, u32 instance);
struct tipc_nlist *tipc_group_dests(struct tipc_group *grp);
void tipc_group_self(struct tipc_group *grp, struct tipc_service_range *seq,
		     int *scope);
u32 tipc_group_exclude(struct tipc_group *grp);
void tipc_group_filter_msg(struct tipc_group *grp,
			   struct sk_buff_head *inputq,
			   struct sk_buff_head *xmitq);
void tipc_group_member_evt(struct tipc_group *grp, bool *wakeup,
			   int *sk_rcvbuf, struct tipc_msg *hdr,
			   struct sk_buff_head *inputq,
			   struct sk_buff_head *xmitq);
void tipc_group_proto_rcv(struct tipc_group *grp, bool *wakeup,
			  struct tipc_msg *hdr,
			  struct sk_buff_head *inputq,
			  struct sk_buff_head *xmitq);
void tipc_group_update_bc_members(struct tipc_group *grp, int len, bool ack);
bool tipc_group_cong(struct tipc_group *grp, u32 dnode, u32 dport,
		     int len, struct tipc_member **m);
bool tipc_group_bc_cong(struct tipc_group *grp, int len);
void tipc_group_update_rcv_win(struct tipc_group *grp, int blks, u32 node,
			       u32 port, struct sk_buff_head *xmitq);
u16 tipc_group_bc_snd_nxt(struct tipc_group *grp);
void tipc_group_update_member(struct tipc_member *m, int len);
int tipc_group_fill_sock_diag(struct tipc_group *grp, struct sk_buff *skb);
#endif
