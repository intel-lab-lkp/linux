// SPDX-License-Identifier: GPL-2.0

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/in.h>
#include <linux/udp.h>
#include <stdbool.h>

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, 256);
	__type(key, __u32);
	__type(value, __u32);
} xsk SEC(".maps");

__u16 port;

SEC("xdp")
int rx(struct xdp_md *ctx)
{
	void *data, *data_end;
	struct ethhdr *eth;
	struct iphdr *iph;
	struct udphdr *uh;

	data = (void *)(long)ctx->data;
	data_end = (void *)(long)ctx->data_end;

	eth = data;
	data = eth + 1;
	if (data > data_end)
		return XDP_PASS;

	if (eth->h_proto == bpf_htons(ETH_P_IP)) {
		iph = data;
		data = iph + 1;
		if (data > data_end)
			return XDP_PASS;
		if (iph->protocol != IPPROTO_UDP)
			return XDP_PASS;
	} else {
		return XDP_PASS;
	}

	uh = data;
	data = uh + 1;
	if (data > data_end)
		return XDP_PASS;
	if (uh->dest != port)
		return XDP_PASS;

	return bpf_redirect_map(&xsk, ctx->rx_queue_index, XDP_PASS);
}

char _license[] SEC("license") = "GPL";
