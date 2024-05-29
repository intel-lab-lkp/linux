// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for the IPPROTO_SMC (socket related)
 *
 *  Copyright IBM Corp. 2016, 2018
 *  Copyright (c) 2024, Alibaba Inc.
 *
 *  Author: D. Wythe <alibuda@linux.alibaba.com>
 */

#include "inet_smc.h"
#include "smc.h"

struct proto smc_inet_prot = {
	.name		= "INET_SMC",
	.owner		= THIS_MODULE,
	.init		= smc_inet_init_sock,
	.hash		= smc_hash_sk,
	.unhash		= smc_unhash_sk,
	.release_cb	= smc_release_cb,
	.obj_size	= sizeof(struct smc_sock),
	.h.smc_hash	= &smc_v4_hashinfo,
	.slab_flags	= SLAB_TYPESAFE_BY_RCU,
};

const struct proto_ops smc_inet_stream_ops = {
	.family		= PF_INET,
	.owner		= THIS_MODULE,
	.release	= smc_release,
	.bind		= smc_bind,
	.connect	= smc_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= smc_accept,
	.getname	= smc_getname,
	.poll		= smc_poll,
	.ioctl		= smc_ioctl,
	.listen		= smc_listen,
	.shutdown	= smc_shutdown,
	.setsockopt	= smc_setsockopt,
	.getsockopt	= smc_getsockopt,
	.sendmsg	= smc_sendmsg,
	.recvmsg	= smc_recvmsg,
	.mmap		= sock_no_mmap,
	.splice_read	= smc_splice_read,
};

struct inet_protosw smc_inet_protosw = {
	.type		= SOCK_STREAM,
	.protocol	= IPPROTO_SMC,
	.prot		= &smc_inet_prot,
	.ops		= &smc_inet_stream_ops,
	.flags		= INET_PROTOSW_ICSK,
};

#if IS_ENABLED(CONFIG_IPV6)
struct proto smc_inet6_prot = {
	.name		= "INET6_SMC",
	.owner		= THIS_MODULE,
	.init		= smc_inet_init_sock,
	.hash		= smc_hash_sk,
	.unhash		= smc_unhash_sk,
	.release_cb	= smc_release_cb,
	.obj_size	= sizeof(struct smc_sock),
	.h.smc_hash	= &smc_v6_hashinfo,
	.slab_flags	= SLAB_TYPESAFE_BY_RCU,
};

const struct proto_ops smc_inet6_stream_ops = {
	.family		= PF_INET6,
	.owner		= THIS_MODULE,
	.release	= smc_release,
	.bind		= smc_bind,
	.connect	= smc_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= smc_accept,
	.getname	= smc_getname,
	.poll		= smc_poll,
	.ioctl		= smc_ioctl,
	.listen		= smc_listen,
	.shutdown	= smc_shutdown,
	.setsockopt	= smc_setsockopt,
	.getsockopt	= smc_getsockopt,
	.sendmsg	= smc_sendmsg,
	.recvmsg	= smc_recvmsg,
	.mmap		= sock_no_mmap,
	.splice_read	= smc_splice_read,
};

struct inet_protosw smc_inet6_protosw = {
	.type		= SOCK_STREAM,
	.protocol	= IPPROTO_SMC,
	.prot		= &smc_inet6_prot,
	.ops		= &smc_inet6_stream_ops,
	.flags		= INET_PROTOSW_ICSK,
};
#endif

int smc_inet_init_sock(struct sock *sk)
{
	struct net *net = sock_net(sk);

	/* init common smc sock */
	smc_sk_init(net, sk, IPPROTO_SMC);
	/* create clcsock */
	return smc_create_clcsk(net, sk, sk->sk_family);
}
