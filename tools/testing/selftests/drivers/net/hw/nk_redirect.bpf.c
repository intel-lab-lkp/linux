// SPDX-License-Identifier: GPL-2.0
/*
 * BPF program for redirecting traffic using bpf_redirect_neigh().
 * Unlike bpf_redirect() which preserves L2 headers, bpf_redirect_neigh()
 * performs neighbor lookup and fills in the correct L2 addresses for the
 * target interface. This is necessary when redirecting across different
 * device types (e.g., from netdevsim to netkit).
 */
#include <linux/bpf.h>
#include <linux/pkt_cls.h>
#include <linux/if_ether.h>
#include <linux/ipv6.h>
#include <linux/in6.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

#define TC_ACT_OK 0
#define ETH_P_IPV6 0x86DD

#define ctx_ptr(field)		((void *)(long)(field))

#define v6_p64_equal(a, b)	(a.s6_addr32[0] == b.s6_addr32[0] && \
				 a.s6_addr32[1] == b.s6_addr32[1])

volatile __u32 redirect_ifindex;
volatile __u8 ipv6_prefix[16];

SEC("tc/ingress")
int tc_redirect(struct __sk_buff *skb)
{
	void *data_end = ctx_ptr(skb->data_end);
	void *data = ctx_ptr(skb->data);
	struct in6_addr *match_prefix;
	struct ipv6hdr *ip6h;
	struct ethhdr *eth;

	match_prefix = (struct in6_addr *)ipv6_prefix;

	if (skb->protocol != bpf_htons(ETH_P_IPV6))
		return TC_ACT_OK;

	eth = data;
	if ((void *)(eth + 1) > data_end)
		return TC_ACT_OK;

	ip6h = data + sizeof(struct ethhdr);
	if ((void *)(ip6h + 1) > data_end)
		return TC_ACT_OK;

	if (!v6_p64_equal(ip6h->daddr, (*match_prefix)))
		return TC_ACT_OK;

	/*
	 * Use bpf_redirect_neigh() to perform neighbor lookup and fill in
	 * correct L2 addresses for the target interface.
	 */
	return bpf_redirect_neigh(redirect_ifindex, NULL, 0, 0);
}

char __license[] SEC("license") = "GPL";
