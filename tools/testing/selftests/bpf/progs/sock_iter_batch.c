// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2024 Meta

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_endian.h>
#include "bpf_tracing_net.h"
#include "bpf_kfuncs.h"

/* __always_inline to avoid the unused function warning for jhash() */
#define ATTR __always_inline
#include "test_jhash.h"

static u32 jhash2(const u32 *k, u32 length, u32 initval)
{
	u32 a, b, c;

	/* Set up the internal state */
	a = b = c = JHASH_INITVAL + (length<<2) + initval;

	/* Handle most of the key */
	while (length > 3) {
		a += k[0];
		b += k[1];
		c += k[2];
		__jhash_mix(a, b, c);
		length -= 3;
		k += 3;
	}

	/* Handle the last 3 u32's */
	switch (length) {
	case 3: c += k[2];
	case 2: b += k[1];
	case 1: a += k[0];
		__jhash_final(a, b, c);
		break;
	case 0:	/* Nothing left to add */
		break;
	}

	return c;
}

static bool ipv6_addr_loopback(const struct in6_addr *a)
{
	return (a->s6_addr32[0] | a->s6_addr32[1] |
		a->s6_addr32[2] | (a->s6_addr32[3] ^ bpf_htonl(1))) == 0;
}

volatile const __u16 ports[2];
unsigned int bucket[2];

SEC("iter/tcp")
int iter_tcp_soreuse(struct bpf_iter__tcp *ctx)
{
	struct sock *sk = (struct sock *)ctx->sk_common;
	struct inet_hashinfo *hinfo;
	unsigned int hash;
	struct net *net;
	int idx;

	if (!sk)
		return 0;

	sk = bpf_rdonly_cast(sk, bpf_core_type_id_kernel(struct sock));
	if (sk->sk_family != AF_INET6 ||
	    sk->sk_state != TCP_LISTEN ||
	    !ipv6_addr_loopback(&sk->sk_v6_rcv_saddr))
		return 0;

	if (sk->sk_num == ports[0])
		idx = 0;
	else if (sk->sk_num == ports[1])
		idx = 1;
	else
		return 0;

	net = sk->sk_net.net;
	hash = jhash2(sk->sk_v6_rcv_saddr.s6_addr32, 4, net->hash_mix);
	hash ^= sk->sk_num;
	hinfo = net->ipv4.tcp_death_row.hashinfo;
	bucket[idx] = hash & hinfo->lhash2_mask;
	bpf_seq_write(ctx->meta->seq, &idx, sizeof(idx));

	return 0;
}

#define udp_sk(ptr) container_of(ptr, struct udp_sock, inet.sk)

SEC("iter/udp")
int iter_udp_soreuse(struct bpf_iter__udp *ctx)
{
	struct sock *sk = (struct sock *)ctx->udp_sk;
	struct udp_table *udptable;
	int idx;

	if (!sk)
		return 0;

	sk = bpf_rdonly_cast(sk, bpf_core_type_id_kernel(struct sock));
	if (sk->sk_family != AF_INET6 ||
	    !ipv6_addr_loopback(&sk->sk_v6_rcv_saddr))
		return 0;

	if (sk->sk_num == ports[0])
		idx = 0;
	else if (sk->sk_num == ports[1])
		idx = 1;
	else
		return 0;

	udptable = sk->sk_net.net->ipv4.udp_table;
	bucket[idx] = udp_sk(sk)->udp_portaddr_hash & udptable->mask;
	bpf_seq_write(ctx->meta->seq, &idx, sizeof(idx));

	return 0;
}

char _license[] SEC("license") = "GPL";
