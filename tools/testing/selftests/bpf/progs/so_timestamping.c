// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Tencent */

#include "vmlinux.h"
#include "bpf_tracing_net.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"

#define SO_TIMESTAMPING 37
#define SOF_TIMESTAMPING_BPF_SUPPPORTED_MASK (SOF_TIMESTAMPING_SOFTWARE | \
					      SOF_TIMESTAMPING_TX_SCHED | \
					      SOF_TIMESTAMPING_TX_SOFTWARE | \
					      SOF_TIMESTAMPING_TX_ACK | \
					      SOF_TIMESTAMPING_OPT_ID | \
					      SOF_TIMESTAMPING_OPT_ID_TCP)

extern unsigned long CONFIG_HZ __kconfig;

int nr_active;
int nr_passive;
int nr_sched;
int nr_txsw;
int nr_ack;

struct sockopt_test {
	int opt;
	int new;
	int expected;
};

static const struct sockopt_test sol_socket_tests[] = {
	{ .opt = SO_TIMESTAMPING, .new = SOF_TIMESTAMPING_TX_SCHED, .expected = 256, },
	{ .opt = SO_TIMESTAMPING, .new = SOF_TIMESTAMPING_BPF_SUPPPORTED_MASK, .expected = 66450, },
	{ .opt = 0, },
};

struct loop_ctx {
	void *ctx;
	struct sock *sk;
};

static int bpf_test_sockopt_int(void *ctx, struct sock *sk,
				const struct sockopt_test *t,
				int level)
{
	int tmp, new, expected, opt;

	opt = t->opt;
	new = t->new;
	expected = t->expected;

	if (bpf_setsockopt(ctx, level, opt, &new, sizeof(new)))
		return 1;
	if (bpf_getsockopt(ctx, level, opt, &tmp, sizeof(tmp)) ||
	    tmp != expected)
		return 1;

	return 0;
}

static int bpf_test_socket_sockopt(__u32 i, struct loop_ctx *lc)
{
	const struct sockopt_test *t;

	if (i >= ARRAY_SIZE(sol_socket_tests))
		return 1;

	t = &sol_socket_tests[i];
	if (!t->opt)
		return 1;

	return bpf_test_sockopt_int(lc->ctx, lc->sk, t, SOL_SOCKET);
}

static int bpf_test_sockopt(void *ctx, struct sock *sk)
{
	struct loop_ctx lc = { .ctx = ctx, .sk = sk, };
	int n;

	n = bpf_loop(ARRAY_SIZE(sol_socket_tests), bpf_test_socket_sockopt, &lc, 0);
	if (n != ARRAY_SIZE(sol_socket_tests))
		return -1;

	return 0;
}

SEC("sockops")
int skops_sockopt(struct bpf_sock_ops *skops)
{
	struct bpf_sock *bpf_sk = skops->sk;
	struct sock *sk;

	if (!bpf_sk)
		return 1;

	sk = (struct sock *)bpf_skc_to_tcp_sock(bpf_sk);
	if (!sk)
		return 1;

	switch (skops->op) {
	case BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB:
		nr_active += !bpf_test_sockopt(skops, sk);
		break;
	case BPF_SOCK_OPS_PASSIVE_ESTABLISHED_CB:
		nr_passive += !bpf_test_sockopt(skops, sk);
		break;
	case BPF_SOCK_OPS_TS_SCHED_OPT_CB:
		nr_sched += 1;
		break;
	case BPF_SOCK_OPS_TS_SW_OPT_CB:
		nr_txsw += 1;
		break;
	case BPF_SOCK_OPS_TS_ACK_OPT_CB:
		nr_ack += 1;
		break;
	}

	return 1;
}

char _license[] SEC("license") = "GPL";
