/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Definitions for the SMC module (socket related)

 *  Copyright IBM Corp. 2016
 *
 */
#ifndef __INET_SMC
#define __INET_SMC

#include <net/protocol.h>
#include <net/sock.h>
#include <net/tcp.h>

extern struct proto smc_inet_prot;
extern const struct proto_ops smc_inet_stream_ops;
extern struct inet_protosw smc_inet_protosw;

#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
/* MUST after net/tcp.h or warning */
#include <net/transp_v6.h>
extern struct proto smc_inet6_prot;
extern const struct proto_ops smc_inet6_stream_ops;
extern struct inet_protosw smc_inet6_protosw;
#endif

int smc_inet_init_sock(struct sock *sk);

#endif // __INET_SMC
