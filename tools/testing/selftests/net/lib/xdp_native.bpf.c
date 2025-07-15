// SPDX-License-Identifier: GPL-2.0

#include <stddef.h>
#include <linux/bpf.h>
#include <linux/in.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define MAX_ADJST_OFFSET 256
#define MAX_PAYLOAD_LEN 9000

enum {
	XDP_MODE = 0,
	XDP_PORT = 1,
	XDP_ADJST_OFFSET = 2,
	XDP_ADJST_TAG = 3,
} xdp_map_setup_keys;

enum {
	XDP_MODE_PASS = 0,
	XDP_MODE_DROP = 1,
	XDP_MODE_TX = 2,
	XDP_MODE_TAIL_ADJST = 3,
} xdp_map_modes;

enum {
	STATS_RX = 0,
	STATS_PASS = 1,
	STATS_DROP = 2,
	STATS_TX = 3,
	STATS_ABORT = 4,
} xdp_stats;

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 5);
	__type(key, __u32);
	__type(value, __s32);
} map_xdp_setup SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 5);
	__type(key, __u32);
	__type(value, __u64);
} map_xdp_stats SEC(".maps");

static inline __u32 min(__u32 a, __u32 b)
{
	return a < b ? a : b;
}

static void record_stats(struct xdp_md *ctx, __u32 stat_type)
{
	__u64 *count;

	count = bpf_map_lookup_elem(&map_xdp_stats, &stat_type);

	if (count)
		__sync_fetch_and_add(count, 1);
}

static struct udphdr *filter_udphdr(struct xdp_md *ctx, __u16 port)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct udphdr *udph = NULL;
	struct ethhdr *eth = data;

	if (data + sizeof(*eth) > data_end)
		return NULL;

	if (eth->h_proto == bpf_htons(ETH_P_IP)) {
		struct iphdr *iph = data + sizeof(*eth);

		if (iph + 1 > (struct iphdr *)data_end ||
		    iph->protocol != IPPROTO_UDP)
			return NULL;

		udph = (void *)eth + sizeof(*iph) + sizeof(*eth);
	} else if (eth->h_proto  == bpf_htons(ETH_P_IPV6)) {
		struct ipv6hdr *ipv6h = data + sizeof(*eth);

		if (ipv6h + 1 > (struct ipv6hdr *)data_end ||
		    ipv6h->nexthdr != IPPROTO_UDP)
			return NULL;

		udph = (void *)eth + sizeof(*ipv6h) + sizeof(*eth);
	} else {
		return NULL;
	}

	if (udph + 1 > (struct udphdr *)data_end)
		return NULL;

	if (udph->dest != bpf_htons(port))
		return NULL;

	record_stats(ctx, STATS_RX);

	return udph;
}

static int xdp_mode_pass(struct xdp_md *ctx, __u16 port)
{
	struct udphdr *udph = NULL;

	udph = filter_udphdr(ctx, port);
	if (!udph)
		return XDP_PASS;

	record_stats(ctx, STATS_PASS);

	return XDP_PASS;
}

static int xdp_mode_drop_handler(struct xdp_md *ctx, __u16 port)
{
	struct udphdr *udph = NULL;

	udph = filter_udphdr(ctx, port);
	if (!udph)
		return XDP_PASS;

	record_stats(ctx, STATS_DROP);

	return XDP_DROP;
}

static void swap_machdr(void *data)
{
	struct ethhdr *eth = data;
	__u8 tmp_mac[ETH_ALEN];

	__builtin_memcpy(tmp_mac, eth->h_source, ETH_ALEN);
	__builtin_memcpy(eth->h_source, eth->h_dest, ETH_ALEN);
	__builtin_memcpy(eth->h_dest, tmp_mac, ETH_ALEN);
}

static int xdp_mode_tx_handler(struct xdp_md *ctx, __u16 port)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct udphdr *udph = NULL;
	struct ethhdr *eth = data;

	if (data + sizeof(*eth) > data_end)
		return XDP_PASS;

	if (eth->h_proto == bpf_htons(ETH_P_IP)) {
		struct iphdr *iph = data + sizeof(*eth);
		__be32 tmp_ip = iph->saddr;

		if (iph + 1 > (struct iphdr *)data_end ||
		    iph->protocol != IPPROTO_UDP)
			return XDP_PASS;

		udph = data + sizeof(*iph) + sizeof(*eth);

		if (udph + 1 > (struct udphdr *)data_end)
			return XDP_PASS;
		if (udph->dest != bpf_htons(port))
			return XDP_PASS;

		record_stats(ctx, STATS_RX);
		swap_machdr((void *)eth);

		iph->saddr = iph->daddr;
		iph->daddr = tmp_ip;

		record_stats(ctx, STATS_TX);

		return XDP_TX;

	} else if (eth->h_proto  == bpf_htons(ETH_P_IPV6)) {
		struct ipv6hdr *ipv6h = data + sizeof(*eth);
		struct in6_addr tmp_ipv6;

		if (ipv6h + 1 > (struct ipv6hdr *)data_end ||
		    ipv6h->nexthdr != IPPROTO_UDP)
			return XDP_PASS;

		udph = data + sizeof(*ipv6h) + sizeof(*eth);

		if (udph + 1 > (struct udphdr *)data_end)
			return XDP_PASS;
		if (udph->dest != bpf_htons(port))
			return XDP_PASS;

		record_stats(ctx, STATS_RX);
		swap_machdr((void *)eth);

		__builtin_memcpy(&tmp_ipv6, &ipv6h->saddr, sizeof(tmp_ipv6));
		__builtin_memcpy(&ipv6h->saddr, &ipv6h->daddr,
				 sizeof(tmp_ipv6));
		__builtin_memcpy(&ipv6h->daddr, &tmp_ipv6, sizeof(tmp_ipv6));

		record_stats(ctx, STATS_TX);

		return XDP_TX;
	}

	return XDP_PASS;
}

static void *update_pkt(void *data, void *data_end, __s16 offset)
{
	struct ethhdr *eth = data;
	struct udphdr *udph = NULL;
	__u16 udp_len = 0;

	if (data + sizeof(*eth) > data_end)
		return NULL;

	if (eth->h_proto == bpf_htons(ETH_P_IP)) {
		struct iphdr *iph = data + sizeof(*eth);
		__u16 total_len;

		if (iph + 1 > (struct iphdr *)data_end)
			return NULL;

		total_len = bpf_ntohs(iph->tot_len) + offset;
		iph->tot_len = bpf_htons(total_len);

		udph = (void *)eth + sizeof(*iph) + sizeof(*eth);
	} else if (eth->h_proto  == bpf_htons(ETH_P_IPV6)) {
		struct ipv6hdr *ipv6h = data + sizeof(*eth);
		__u16 payload_len;

		if (ipv6h + 1 > (struct ipv6hdr *)data_end)
			return NULL;

		payload_len = bpf_ntohs(ipv6h->payload_len) + offset;
		ipv6h->payload_len = bpf_htons(payload_len);

		udph = (void *)eth + sizeof(*ipv6h) + sizeof(*eth);
	} else {
		return NULL;
	}

	if (!udph || udph + 1 > (struct udphdr *)data_end)
		return NULL;

	udp_len = bpf_ntohs(udph->len) + offset;
	udph->len = bpf_htons(udp_len);

	return udph;
}

static int xdp_tail_ext(struct xdp_md *ctx, __u16 port)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct udphdr *udph = NULL;
	__s32 *adjust_offset, *val;
	void *offset_ptr;
	__u32 key;
	__u8 tag;

	udph = filter_udphdr(ctx, port);
	if (!udph)
		return XDP_PASS;

	key = XDP_ADJST_OFFSET;
	adjust_offset = bpf_map_lookup_elem(&map_xdp_setup, &key);
	if (!adjust_offset)
		return XDP_PASS;

	/* Only attempt to shrink the data part */
	if (*adjust_offset > bpf_ntohs(udph->len))
		goto abort_pkt;

	if (bpf_xdp_adjust_tail(ctx, 0 - *adjust_offset) < 0)
		goto abort_pkt;

	key = XDP_ADJST_TAG;
	val = bpf_map_lookup_elem(&map_xdp_setup, &key);
	if (!val)
		goto abort_pkt;
	tag = (__u8)(*val);

	data = (void *)(long)ctx->data;
	data_end = (void *)(long)ctx->data_end;

	udph = update_pkt(data, data_end, (__s16)(0 - *adjust_offset));
	if (!udph)
		goto abort_pkt;

	/* For the tail-shrink case, we can simply proceed to passing
	 * the packet up to the stack. For the tail-growth case, we
	 * insert appropriate tags at the start and end of the newly
	 * created space and then validate these tags in the BPF program
	 */
	if (*adjust_offset > 0)
		goto pass_pkt;

	__u32 pkt_offset = bpf_ntohs(udph->len) - (__u32)(0 - *adjust_offset);

	/* The min operations here set the upper bound on the adjustment
	 * offset and offset_ptr allowing us to pass the BPF verifier check
	 */
	offset_ptr = (void *)udph + min(MAX_PAYLOAD_LEN, pkt_offset);
	for (int i = 0; i < MAX_ADJST_OFFSET; i++) {
		if (offset_ptr == (void *)udph + min(MAX_PAYLOAD_LEN,
						     bpf_ntohs(udph->len)))
			break;

		if ((void *)(offset_ptr + 1) > data_end)
			goto abort_pkt;

		__builtin_memcpy(offset_ptr, &tag, 1);

		offset_ptr++;
	}

pass_pkt:
	record_stats(ctx, STATS_PASS);

	return XDP_PASS;

abort_pkt:
	record_stats(ctx, STATS_ABORT);

	return XDP_ABORTED;
}

static int xdp_prog_common(struct xdp_md *ctx)
{
	__u32 key, *port;
	__s32 *mode;

	key = XDP_MODE;
	mode = bpf_map_lookup_elem(&map_xdp_setup, &key);
	if (!mode)
		return XDP_PASS;

	key = XDP_PORT;
	port = bpf_map_lookup_elem(&map_xdp_setup, &key);
	if (!port)
		return XDP_PASS;

	switch (*mode) {
	case XDP_MODE_PASS:
		return xdp_mode_pass(ctx, (__u16)(*port));
	case XDP_MODE_DROP:
		return xdp_mode_drop_handler(ctx, (__u16)(*port));
	case XDP_MODE_TX:
		return xdp_mode_tx_handler(ctx, (__u16)(*port));
	case XDP_MODE_TAIL_ADJST:
		return xdp_tail_ext(ctx, (__u16)(*port));
	}

	/* Default action is to simple pass */
	return XDP_PASS;
}

SEC("xdp")
int xdp_prog(struct xdp_md *ctx)
{
	return xdp_prog_common(ctx);
}

SEC("xdp.frags")
int xdp_prog_frags(struct xdp_md *ctx)
{
	return xdp_prog_common(ctx);
}

char _license[] SEC("license") = "GPL";
