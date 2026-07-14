// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

#define AF_INET 2

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_SOCKHASH);
	__uint(max_entries, 1024);
	__type(key, __u64);
	__type(value, __u64);

} map_socks SEC(".maps");

SEC("sockops") int on_sockops(struct bpf_sock_ops *ctx)
{
	if (ctx->family == AF_INET && ctx->op == BPF_SOCK_OPS_ACTIVE_ESTABLISHED_CB) {
		__u64 cookie = bpf_get_socket_cookie(ctx);

		bpf_sock_hash_update(ctx, &map_socks, &cookie, BPF_NOEXIST);
	}

	return 0;
}

SEC("sk_skb/stream_verdict") int on_recv(struct __sk_buff *ctx)
{
	return SK_PASS;
}
