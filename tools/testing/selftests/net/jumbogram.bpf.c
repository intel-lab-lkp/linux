// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} max_packet_size SEC(".maps");

SEC("ingress")
int track_max_size(struct __sk_buff *skb)
{
	__u32 *max_size_ptr, *count;
	__u32 max_size;
	__u32 key = 0;

	max_size_ptr = bpf_map_lookup_elem(&max_packet_size, &key);
	if (max_size_ptr)
		max_size = *max_size_ptr;
	else
		max_size = 0;

	if (skb->len >= max_size) {
		max_size = skb->len;
		bpf_map_update_elem(&max_packet_size, &key, &max_size,
				    BPF_ANY);
	}

	return TC_ACT_OK;
}

char _license[] SEC("license") = ("GPL");
