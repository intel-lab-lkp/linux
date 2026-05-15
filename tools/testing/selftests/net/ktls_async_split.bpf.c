// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

#ifndef SK_PASS
#define SK_PASS 1
#endif

char LICENSE[] SEC("license") = "GPL";

struct {
	__uint(type, BPF_MAP_TYPE_SOCKMAP);
	__uint(max_entries, 4);
	__type(key, __u32);
	__type(value, int);
} sock_map SEC(".maps");

SEC("sk_msg")
int apply_bytes_verdict(struct sk_msg_md *msg)
{
	bpf_msg_apply_bytes(msg, 512);
	return SK_PASS;
}
