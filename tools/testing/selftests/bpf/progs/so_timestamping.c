// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Tencent */

#include "vmlinux.h"
#include "bpf_tracing_net.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "bpf_misc.h"

#define SK_BPF_CB_FLAGS 1009
#define SK_BPF_CB_TX_TIMESTAMPING 1

int nr_active;
int nr_passive;
int nr_sched;
int nr_txsw;
int nr_ack;

struct sockopt_test {
	int opt;
	int new;
};

static const struct sockopt_test sol_socket_tests[] = {
	{ .opt = SK_BPF_CB_FLAGS, .new = SK_BPF_CB_TX_TIMESTAMPING, },
	{ .opt = 0, },
};

struct loop_ctx {
	void *ctx;
	struct sock *sk;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, u32);
	__type(value, u64);
	__uint(max_entries, 1024);
} hash_map SEC(".maps");

static u64 delay_tolerance_nsec = 5000000;

static int bpf_test_sockopt_int(void *ctx, struct sock *sk,
				const struct sockopt_test *t,
				int level)
{
	int new, opt;

	opt = t->opt;
	new = t->new;

	if (bpf_setsockopt(ctx, level, opt, &new, sizeof(new)))
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

static bool bpf_test_delay(struct bpf_sock_ops *skops)
{
	u64 timestamp = bpf_ktime_get_ns();
	u32 seq = skops->args[2];
	u64 *value;

	value = bpf_map_lookup_elem(&hash_map, &seq);
	if (value && (timestamp - *value > delay_tolerance_nsec)) {
		bpf_printk("time delay: %lu", timestamp - *value);
		return false;
	}

	bpf_map_update_elem(&hash_map, &seq, &timestamp, BPF_ANY);
	return true;
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
	case BPF_SOCK_OPS_TS_SCHED_OPT_CB:
		if (bpf_test_delay(skops))
			nr_sched += 1;
		break;
	case BPF_SOCK_OPS_TS_SW_OPT_CB:
		if (bpf_test_delay(skops))
			nr_txsw += 1;
		break;
	case BPF_SOCK_OPS_TS_ACK_OPT_CB:
		if (bpf_test_delay(skops))
			nr_ack += 1;
		break;
	}

	return 1;
}

char _license[] SEC("license") = "GPL";
