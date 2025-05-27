// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, 1);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(__u32));
} xsk_map SEC(".maps");

SEC("xdp.frags")
int dummy_prog(struct xdp_md *ctx)
{
	return XDP_PASS;
}

SEC("xdp.frags")
int redirect_xsk_prog(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	struct iphdr *iph;

	if (data + sizeof(*eth) + sizeof(*iph) > data_end)
		return XDP_PASS;

	if (bpf_htons(eth->h_proto) != ETH_P_IP)
		return XDP_PASS;

	iph = data + sizeof(*eth);
	if (iph->protocol != IPPROTO_UDP)
		return XDP_PASS;

	return bpf_redirect_map(&xsk_map, 0, XDP_DROP);
}

char _license[] SEC("license") = "GPL";
