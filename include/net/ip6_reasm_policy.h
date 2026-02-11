/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NET_IP6_REASM_POLICY_H
#define _NET_IP6_REASM_POLICY_H

struct sk_buff;
struct frag_queue;

void ip6_reasm_adjust_timer(struct frag_queue *fq,
			    struct sk_buff *skb, int nhoff);

/*
 * Default IPv6 reassembly timeouts under fragment memory pressure
 */
#define IPV6_REASM_TIMEOUT_FAILED_TCP	3	/* 3 seconds */
#define IPV6_REASM_TIMEOUT_FAILED_UDP	1	/* 1 second */

#endif /* _NET_IP6_REASM_POLICY_H */
