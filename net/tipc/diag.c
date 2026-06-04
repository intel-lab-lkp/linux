// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * net/tipc/diag.c: TIPC socket diag
 *
 * Copyright (c) 2018, Ericsson AB
 * All rights reserved.
 */

#include "core.h"
#include "socket.h"
#include <linux/sock_diag.h>
#include <linux/tipc_sockets_diag.h>

static u64 __tipc_diag_gen_cookie(struct sock *sk)
{
	u32 res[2];

	sock_diag_save_cookie(sk, res);
	return *((u64 *)res);
}

static int __tipc_add_sock_diag(struct sk_buff *skb,
				struct netlink_callback *cb,
				struct tipc_sock *tsk)
{
	struct tipc_sock_diag_req *req = nlmsg_data(cb->nlh);
	struct nlmsghdr *nlh;
	int err;

	nlh = nlmsg_put_answer(skb, cb, SOCK_DIAG_BY_FAMILY, 0,
			       NLM_F_MULTI);
	if (!nlh)
		return -EMSGSIZE;

	err = tipc_sk_fill_sock_diag(skb, cb, tsk, req->tidiag_states,
				     __tipc_diag_gen_cookie);
	if (err)
		return err;

	nlmsg_end(skb, nlh);
	return 0;
}

static int tipc_diag_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	return tipc_nl_sk_walk(skb, cb, __tipc_add_sock_diag);
}

static int tipc_sock_diag_handler_dump(struct sk_buff *skb,
				       struct nlmsghdr *h)
{
	int hdrlen = sizeof(struct tipc_sock_diag_req);
	struct net *net = sock_net(skb->sk);

	if (nlmsg_len(h) < hdrlen)
		return -EINVAL;

	if (h->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = tipc_dump_start,
			.dump = tipc_diag_dump,
			.done = tipc_dump_done,
		};
		netlink_dump_start(net->diag_nlsk, skb, h, &c);
		return 0;
	}
	return -EOPNOTSUPP;
}

static const struct sock_diag_handler tipc_sock_diag_handler = {
	.owner = THIS_MODULE,
	.family = AF_TIPC,
	.dump = tipc_sock_diag_handler_dump,
};

static int __init tipc_diag_init(void)
{
	return sock_diag_register(&tipc_sock_diag_handler);
}

static void __exit tipc_diag_exit(void)
{
	sock_diag_unregister(&tipc_sock_diag_handler);
}

module_init(tipc_diag_init);
module_exit(tipc_diag_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TIPC socket monitoring via SOCK_DIAG");
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_NETLINK, NETLINK_SOCK_DIAG, AF_TIPC);
