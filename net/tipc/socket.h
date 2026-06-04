/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/* net/tipc/socket.h: Include file for TIPC socket code
 *
 * Copyright (c) 2014-2016, Ericsson AB
 * All rights reserved.
 */

#ifndef _TIPC_SOCK_H
#define _TIPC_SOCK_H

#include <net/sock.h>
#include <net/genetlink.h>

/* Compatibility values for deprecated message based flow control */
#define FLOWCTL_MSG_WIN 512
#define FLOWCTL_MSG_LIM ((FLOWCTL_MSG_WIN * 2 + 1) * SKB_TRUESIZE(MAX_MSG_SIZE))

#define FLOWCTL_BLK_SZ 1024

/* Socket receive buffer sizes */
#define RCVBUF_MIN  (FLOWCTL_BLK_SZ * 512)
#define RCVBUF_DEF  (FLOWCTL_BLK_SZ * 1024 * 2)
#define RCVBUF_MAX  (FLOWCTL_BLK_SZ * 1024 * 16)

struct tipc_sock;

int tipc_socket_init(void);
void tipc_socket_stop(void);
void tipc_sk_rcv(struct net *net, struct sk_buff_head *inputq);
void tipc_sk_mcast_rcv(struct net *net, struct sk_buff_head *arrvq,
		       struct sk_buff_head *inputq);
void tipc_sk_reinit(struct net *net);
int tipc_sk_rht_init(struct net *net);
void tipc_sk_rht_destroy(struct net *net);
int tipc_nl_sk_dump(struct sk_buff *skb, struct netlink_callback *cb);
int tipc_nl_publ_dump(struct sk_buff *skb, struct netlink_callback *cb);
int tipc_sk_fill_sock_diag(struct sk_buff *skb, struct netlink_callback *cb,
			   struct tipc_sock *tsk, u32 sk_filter_state,
			   u64 (*tipc_diag_gen_cookie)(struct sock *sk));
int tipc_nl_sk_walk(struct sk_buff *skb, struct netlink_callback *cb,
		    int (*skb_handler)(struct sk_buff *skb,
				       struct netlink_callback *cb,
				       struct tipc_sock *tsk));
int tipc_dump_start(struct netlink_callback *cb);
int __tipc_dump_start(struct netlink_callback *cb, struct net *net);
int tipc_dump_done(struct netlink_callback *cb);
u32 tipc_sock_get_portid(struct sock *sk);
bool tipc_sk_overlimit1(struct sock *sk, struct sk_buff *skb);
bool tipc_sk_overlimit2(struct sock *sk, struct sk_buff *skb);
int tipc_sk_bind(struct socket *sock, struct sockaddr *skaddr, int alen);
int tsk_set_importance(struct sock *sk, int imp);

#endif
