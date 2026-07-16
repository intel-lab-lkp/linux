// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit equivalence tests for the flow dissector opt-in fast paths.
 *
 * Every fast-path shape contracts to be byte-identical with the slow
 * path. Each corpus packet is dissected twice into zeroed containers
 * -- gates off, then gates on -- and the return values, the full
 * struct flow_keys and flow_hash_from_keys() must match exactly, for
 * both eligible dissectors (flow_keys_dissector and the file-static
 * symmetric one). (A fast-path miss cannot leak partial writes to
 * callers: flow_dissect_fast() is only invoked from
 * __skb_flow_dissect(), which continues into the full slow-path walk
 * on the same container whenever the fast path returns false.)
 */

#include <kunit/test.h>

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_pppox.h>
#include <linux/if_tunnel.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/mpls.h>
#include <linux/netdevice.h>
#include <linux/ppp_defs.h>
#include <linux/prandom.h>
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <net/flow_dissector.h>
#include <net/gre.h>
#include <net/ip.h>
#include <net/net_namespace.h>

#define FD_TEST_BUF_LEN 512
#define FD_FUZZ_ITERS 4000

/* The byte-identical fast-path gates. */
static struct static_key_false * const fd_fast_gates[] = {
	&flow_dissector_eth_ip_key,
	&flow_dissector_vlan_key,
	&flow_dissector_qinq_key,
	&flow_dissector_pppoe_key,
	&flow_dissector_mpls_key,
	&flow_dissector_ipip_key,
	&flow_dissector_gre_key,
};

static void fd_fast_gates_set(bool on)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fd_fast_gates); i++) {
		if (on)
			static_branch_enable(fd_fast_gates[i]);
		else
			static_branch_disable(fd_fast_gates[i]);
	}
}

/* --- packet builders (network-layer payloads; __skb_flow_dissect is
 * called in raw-data mode with the L3 proto passed explicitly, so no
 * Ethernet header is included) ---
 */

static int put_ports(u8 *buf)
{
	__be16 *p = (__be16 *)buf;

	p[0] = htons(0x1234);
	p[1] = htons(0x5678);
	return 4;
}

static int put_ipv4(u8 *buf, u8 ip_proto, u8 ihl, __be16 frag_off)
{
	struct iphdr *ip = (struct iphdr *)buf;
	int hdrlen = ihl * 4;

	memset(ip, 0, hdrlen);
	ip->version = 4;
	ip->ihl = ihl;
	ip->ttl = 64;
	ip->tot_len = htons(hdrlen + 4);
	ip->frag_off = frag_off;
	ip->protocol = ip_proto;
	ip->saddr = htonl(0xc0a80101);	/* 192.168.1.1 */
	ip->daddr = htonl(0xc0a80202);	/* 192.168.2.2 */
	return hdrlen;
}

static int put_ipv6(u8 *buf, u8 nexthdr, u32 flow_label)
{
	struct ipv6hdr *ip6 = (struct ipv6hdr *)buf;

	memset(ip6, 0, sizeof(*ip6));
	ip6->version = 6;
	ip6->flow_lbl[0] = (flow_label >> 16) & 0x0f;
	ip6->flow_lbl[1] = (flow_label >> 8) & 0xff;
	ip6->flow_lbl[2] = flow_label & 0xff;
	ip6->payload_len = htons(4);
	ip6->nexthdr = nexthdr;
	ip6->hop_limit = 64;
	ip6->saddr.s6_addr32[0] = htonl(0x20010db8);
	ip6->saddr.s6_addr32[3] = htonl(1);
	ip6->daddr.s6_addr32[0] = htonl(0x20010db8);
	ip6->daddr.s6_addr32[3] = htonl(2);
	return sizeof(*ip6);
}

static int put_vlan(u8 *buf, u16 tci, __be16 inner_proto)
{
	struct vlan_hdr *vlan = (struct vlan_hdr *)buf;

	vlan->h_vlan_TCI = htons(tci);
	vlan->h_vlan_encapsulated_proto = inner_proto;
	return sizeof(*vlan);
}

static int put_pppoe(u8 *buf, u16 ppp_proto, u16 payload_len)
{
	struct pppoe_hdr *hdr = (struct pppoe_hdr *)buf;
	__be16 *proto = (__be16 *)(buf + sizeof(*hdr));

	memset(hdr, 0, sizeof(*hdr));
	hdr->ver = 1;
	hdr->type = 1;
	hdr->code = 0;
	hdr->sid = htons(0x42);
	hdr->length = htons(payload_len + 2);
	*proto = htons(ppp_proto);
	return sizeof(*hdr) + 2;
}

static int put_mpls(u8 *buf, u32 label, u32 bos)
{
	struct mpls_label *lse = (struct mpls_label *)buf;

	lse->entry = htonl((label << MPLS_LS_LABEL_SHIFT) |
			   (bos << MPLS_LS_S_SHIFT) |
			   (64 << MPLS_LS_TTL_SHIFT));
	return sizeof(*lse);
}

static int put_gre(u8 *buf, __be16 flags, __be16 protocol)
{
	struct gre_base_hdr *gre = (struct gre_base_hdr *)buf;

	gre->flags = flags;
	gre->protocol = protocol;
	return sizeof(*gre);
}

/* --- composite builders, one per corpus entry --- */

static int build_ipv4_tcp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_TCP, 5, 0);

	return n + put_ports(buf + n);
}

static int build_ipv4_udp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_UDP, 5, 0);

	return n + put_ports(buf + n);
}

static int build_ipv6_tcp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_TCP, 0);

	return n + put_ports(buf + n);
}

static int build_ipv6_udp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_UDP, 0);

	return n + put_ports(buf + n);
}

static int build_vlan_ipv4_tcp(u8 *buf)
{
	int n = put_vlan(buf, 100, htons(ETH_P_IP));

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_vlan_ipv6_udp(u8 *buf)
{
	int n = put_vlan(buf, 100, htons(ETH_P_IPV6));

	n += put_ipv6(buf + n, IPPROTO_UDP, 0);
	return n + put_ports(buf + n);
}

static int build_qinq_ipv4_tcp(u8 *buf)
{
	int n = put_vlan(buf, 100, htons(ETH_P_8021Q));

	n += put_vlan(buf + n, 200, htons(ETH_P_IP));
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_vlan3_ipv4_tcp(u8 *buf)
{
	int n = put_vlan(buf, 100, htons(ETH_P_8021Q));

	n += put_vlan(buf + n, 200, htons(ETH_P_8021Q));
	n += put_vlan(buf + n, 300, htons(ETH_P_IP));
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_pppoe_ipv4_tcp(u8 *buf)
{
	int n = put_pppoe(buf, PPP_IP, 20 + 4);

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_pppoe_ipv6_tcp(u8 *buf)
{
	int n = put_pppoe(buf, PPP_IPV6, 40 + 4);

	n += put_ipv6(buf + n, IPPROTO_TCP, 0);
	return n + put_ports(buf + n);
}

static int build_pppoe_lcp(u8 *buf)
{
	int n = put_pppoe(buf, PPP_LCP, 4);
	u8 lcp[4] = { 0x01, 0x01, 0x00, 0x04 };

	memcpy(buf + n, lcp, sizeof(lcp));
	return n + sizeof(lcp);
}

static int build_mpls_ipv4(u8 *buf)
{
	int n = put_mpls(buf, 1000, 1);

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_mpls_multi(u8 *buf)
{
	int n = put_mpls(buf, 1000, 0);	/* S=0: not bottom of stack */

	n += put_mpls(buf + n, 2000, 1);
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_ipip(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_IPIP, 5, 0);

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_6in4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_IPV6, 5, 0);

	n += put_ipv6(buf + n, IPPROTO_TCP, 0);
	return n + put_ports(buf + n);
}

static int build_4in6(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_IPIP, 0);

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_4in6_flowlabel(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_IPIP, 0xbeef);

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_6in6(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_IPV6, 0);

	n += put_ipv6(buf + n, IPPROTO_TCP, 0);
	return n + put_ports(buf + n);
}

static int build_gre_ipv4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, 0, htons(ETH_P_IP));
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_ipv6(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, 0, htons(ETH_P_IPV6));
	n += put_ipv6(buf + n, IPPROTO_TCP, 0);
	return n + put_ports(buf + n);
}

static int build_gre6_ipv6(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_GRE, 0);

	n += put_gre(buf + n, 0, htons(ETH_P_IPV6));
	n += put_ipv6(buf + n, IPPROTO_TCP, 0);
	return n + put_ports(buf + n);
}

static int build_gre6_ipv4(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_GRE, 0);

	n += put_gre(buf + n, 0, htons(ETH_P_IP));
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

/* --- corner-case builders --- */

/* GRE variants that must defer (any non-zero flags/version word). */
static int build_gre_seq_ipv4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, GRE_SEQ, htons(ETH_P_IP));
	memset(buf + n, 0, 4);		/* sequence number */
	n += 4;
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_key_ipv4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, GRE_KEY, htons(ETH_P_IP));
	memset(buf + n, 0, 4);		/* key */
	n += 4;
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_version1(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	/* GRE version 1 (PPTP) lives in the low bits of the flags word. */
	n += put_gre(buf + n, htons(0x0001), htons(ETH_P_IP));
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_teb(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	/* Transparent Ethernet bridging inner: fast path defers. */
	n += put_gre(buf + n, 0, htons(ETH_P_TEB));
	return n + put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
}

/* An IPv6 extension header (8 bytes, hdrlen field 0). */
static int put_ext_hdr(u8 *buf, u8 next_proto)
{
	memset(buf, 0, 8);
	buf[0] = next_proto;
	buf[1] = 0;			/* hdr ext len: (0 + 1) * 8 = 8 bytes */
	return 8;
}

static int build_ipv6_hopopts_dest_tcp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_HOPOPTS, 0);

	n += put_ext_hdr(buf + n, IPPROTO_DSTOPTS);
	n += put_ext_hdr(buf + n, IPPROTO_TCP);
	return n + put_ports(buf + n);
}

static int build_ipv6_frag_tcp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_FRAGMENT, 0);

	memset(buf + n, 0, 8);
	buf[n] = IPPROTO_TCP;		/* fragment header, offset 0 */
	*(__be32 *)(buf + n + 4) = htonl(0x12345678);
	n += 8;
	return n + put_ports(buf + n);
}

/* TPID ordering not covered elsewhere: 8021Q outer, 8021AD inner. */
static int build_vlan_q_then_ad_ipv4(u8 *buf)
{
	int n = put_vlan(buf, 100, htons(ETH_P_8021AD));

	n += put_vlan(buf + n, 200, htons(ETH_P_IP));
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

/* IPv4 with a larger options block (IHL 8 => 12 bytes of options). */
static int build_ipv4_options12_tcp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_TCP, 8, 0);

	return n + put_ports(buf + n);
}

/* PPPoE with a PFC-compressed (odd MSB) PPP protocol field: defers. */
static int build_pppoe_pfc(u8 *buf)
{
	int n = put_pppoe(buf, 0x2100, 20 + 4);

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

/* Mid-stream fragment: non-zero offset AND MF still set. */
static int build_ipv4_frag_mid_tcp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_TCP, 5, htons(IP_MF | 5));

	return n + put_ports(buf + n);
}

/* GRE optional-field combinations (order: csum+reserved, key, seq). */
static int build_gre_csum_key_ipv4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, GRE_CSUM | GRE_KEY, htons(ETH_P_IP));
	memset(buf + n, 0, 8);		/* checksum + reserved, key */
	n += 8;
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_csum_key_seq_ipv4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, GRE_CSUM | GRE_KEY | GRE_SEQ, htons(ETH_P_IP));
	memset(buf + n, 0, 12);		/* checksum + reserved, key, seq */
	n += 12;
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

/* Priority-tagged frame (802.1Q VID 0, only PCP bits in the TCI). */
static int build_vlan_prio_ipv4_tcp(u8 *buf)
{
	int n = put_vlan(buf, 5 << VLAN_PRIO_SHIFT, htons(ETH_P_IP));

	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

/* IPv6 routing extension header before TCP. */
static int build_ipv6_routing_tcp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_ROUTING, 0);

	n += put_ext_hdr(buf + n, IPPROTO_TCP);
	return n + put_ports(buf + n);
}

/* Deeply nested IPIP / GRE chains for the recursion cap. */
static int build_ipip_nest(u8 *buf, int levels)
{
	int n = 0, i;

	for (i = 0; i < levels; i++)
		n += put_ipv4(buf + n, IPPROTO_IPIP, 5, 0);
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_nest(u8 *buf, int levels)
{
	int n = 0, i;

	for (i = 0; i < levels; i++) {
		n += put_ipv4(buf + n, IPPROTO_GRE, 5, 0);
		n += put_gre(buf + n, 0, htons(ETH_P_IP));
	}
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_gre_csum_ipv4(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_GRE, 5, 0);

	n += put_gre(buf + n, GRE_CSUM, htons(ETH_P_IP));
	memset(buf + n, 0, 4);		/* checksum + reserved */
	n += 4;
	n += put_ipv4(buf + n, IPPROTO_TCP, 5, 0);
	return n + put_ports(buf + n);
}

static int build_ipv4_options_tcp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_TCP, 6, 0);	/* IHL=6: 4B options */

	return n + put_ports(buf + n);
}

static int build_ipv4_frag_tcp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_TCP, 5, htons(IP_MF));

	return n + put_ports(buf + n);
}

static int build_ipv4_frag_offset_tcp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_TCP, 5, htons(0x0010));

	return n + put_ports(buf + n);
}

static int build_ipv4_icmp(u8 *buf)
{
	int n = put_ipv4(buf, IPPROTO_ICMP, 5, 0);
	u8 icmp[8] = { 0x08, 0x00, 0x00, 0x00, 0x12, 0x34, 0x00, 0x01 };

	memcpy(buf + n, icmp, sizeof(icmp));
	return n + sizeof(icmp);
}

static int build_ipv6_hopopts_tcp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_HOPOPTS, 0);
	u8 hbh[8] = { IPPROTO_TCP, 0, 0, 0, 0, 0, 0, 0 };

	memcpy(buf + n, hbh, sizeof(hbh));
	n += sizeof(hbh);
	return n + put_ports(buf + n);
}

static int build_ipv6_flowlabel_tcp(u8 *buf)
{
	int n = put_ipv6(buf, IPPROTO_TCP, 0xbeef);

	return n + put_ports(buf + n);
}

struct fd_fast_case {
	const char *name;
	int (*build)(u8 *buf);
	__be16 proto;
	unsigned int flags;
};

/* htons() is not constant-foldable here without cpu_to_be16 literals;
 * use cpu_to_be16 for static initializers.
 */
static const struct fd_fast_case fd_fast_cases[] = {
	/* eligible shapes: the fast path should take these */
	{ "ipv4_tcp", build_ipv4_tcp, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv4_udp", build_ipv4_udp, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv6_tcp", build_ipv6_tcp, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "ipv6_udp", build_ipv6_udp, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "vlan_ipv4_tcp", build_vlan_ipv4_tcp, cpu_to_be16(ETH_P_8021Q), 0 },
	{ "vlan_ipv6_udp", build_vlan_ipv6_udp, cpu_to_be16(ETH_P_8021Q), 0 },
	{ "qinq_ipv4_tcp", build_qinq_ipv4_tcp, cpu_to_be16(ETH_P_8021AD), 0 },
	{ "pppoe_ipv4_tcp", build_pppoe_ipv4_tcp, cpu_to_be16(ETH_P_PPP_SES), 0 },
	{ "pppoe_ipv6_tcp", build_pppoe_ipv6_tcp, cpu_to_be16(ETH_P_PPP_SES), 0 },
	{ "mpls_ipv4", build_mpls_ipv4, cpu_to_be16(ETH_P_MPLS_UC), 0 },
	{ "ipip", build_ipip, cpu_to_be16(ETH_P_IP), 0 },
	{ "6in4", build_6in4, cpu_to_be16(ETH_P_IP), 0 },
	{ "4in6", build_4in6, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "4in6_flowlabel", build_4in6_flowlabel, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "6in6", build_6in6, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "gre_ipv4", build_gre_ipv4, cpu_to_be16(ETH_P_IP), 0 },
	{ "gre6_ipv4", build_gre6_ipv4, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "gre_ipv6", build_gre_ipv6, cpu_to_be16(ETH_P_IP), 0 },
	{ "gre6_ipv6", build_gre6_ipv6, cpu_to_be16(ETH_P_IPV6), 0 },
	/* deliberate fast-path misses: must fall back with identical
	 * output (and, per the zeroed containers, no partial writes)
	 */
	{ "ipv4_options_tcp", build_ipv4_options_tcp, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv4_frag_mf", build_ipv4_frag_tcp, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv4_frag_mf_1stfrag", build_ipv4_frag_tcp, cpu_to_be16(ETH_P_IP),
	  FLOW_DISSECTOR_F_PARSE_1ST_FRAG },
	{ "ipv4_frag_offset", build_ipv4_frag_offset_tcp,
	  cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv4_icmp", build_ipv4_icmp, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv6_hopopts_tcp", build_ipv6_hopopts_tcp,
	  cpu_to_be16(ETH_P_IPV6), 0 },
	{ "ipv6_flowlabel_tcp", build_ipv6_flowlabel_tcp,
	  cpu_to_be16(ETH_P_IPV6), 0 },
	{ "vlan3_ipv4_tcp", build_vlan3_ipv4_tcp, cpu_to_be16(ETH_P_8021Q), 0 },
	{ "pppoe_lcp", build_pppoe_lcp, cpu_to_be16(ETH_P_PPP_SES), 0 },
	{ "mpls_multi_label", build_mpls_multi, cpu_to_be16(ETH_P_MPLS_UC), 0 },
	{ "gre_csum_ipv4", build_gre_csum_ipv4, cpu_to_be16(ETH_P_IP), 0 },
	/* corner cases */
	{ "gre_seq_ipv4", build_gre_seq_ipv4, cpu_to_be16(ETH_P_IP), 0 },
	{ "gre_key_ipv4", build_gre_key_ipv4, cpu_to_be16(ETH_P_IP), 0 },
	{ "gre_version1", build_gre_version1, cpu_to_be16(ETH_P_IP), 0 },
	{ "gre_teb", build_gre_teb, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv6_hopopts_dest_tcp", build_ipv6_hopopts_dest_tcp,
	  cpu_to_be16(ETH_P_IPV6), 0 },
	{ "ipv6_frag_tcp", build_ipv6_frag_tcp, cpu_to_be16(ETH_P_IPV6), 0 },
	{ "vlan_q_then_ad_ipv4", build_vlan_q_then_ad_ipv4,
	  cpu_to_be16(ETH_P_8021Q), 0 },
	{ "ipv4_options12_tcp", build_ipv4_options12_tcp,
	  cpu_to_be16(ETH_P_IP), 0 },
	{ "pppoe_pfc", build_pppoe_pfc, cpu_to_be16(ETH_P_PPP_SES), 0 },
	{ "ipv4_frag_mid", build_ipv4_frag_mid_tcp, cpu_to_be16(ETH_P_IP), 0 },
	{ "ipv4_frag_mid_1stfrag", build_ipv4_frag_mid_tcp,
	  cpu_to_be16(ETH_P_IP), FLOW_DISSECTOR_F_PARSE_1ST_FRAG },
	{ "gre_csum_key_ipv4", build_gre_csum_key_ipv4,
	  cpu_to_be16(ETH_P_IP), 0 },
	{ "gre_csum_key_seq_ipv4", build_gre_csum_key_seq_ipv4,
	  cpu_to_be16(ETH_P_IP), 0 },
	{ "vlan_prio_ipv4_tcp", build_vlan_prio_ipv4_tcp,
	  cpu_to_be16(ETH_P_8021Q), 0 },
	{ "ipv6_routing_tcp", build_ipv6_routing_tcp,
	  cpu_to_be16(ETH_P_IPV6), 0 },
};

static void fd_fast_case_to_desc(const struct fd_fast_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(fd_fast, fd_fast_cases, fd_fast_case_to_desc);

/* Dissect with gates off then on; returns and output must match. */
static void fd_check_one(struct kunit *test, struct flow_dissector *fd,
			 const u8 *data, __be16 proto, int hlen,
			 unsigned int flags)
{
	struct flow_keys keys_slow, keys_fast;
	bool ret_slow, ret_fast;

	fd_fast_gates_set(false);
	memset(&keys_slow, 0, sizeof(keys_slow));
	ret_slow = __skb_flow_dissect(&init_net, NULL, fd, &keys_slow,
				      data, proto, 0, hlen, flags);

	fd_fast_gates_set(true);
	memset(&keys_fast, 0, sizeof(keys_fast));
	ret_fast = __skb_flow_dissect(&init_net, NULL, fd, &keys_fast,
				      data, proto, 0, hlen, flags);
	fd_fast_gates_set(false);

	KUNIT_EXPECT_EQ(test, ret_slow, ret_fast);
	KUNIT_EXPECT_MEMEQ(test, &keys_slow, &keys_fast, sizeof(keys_slow));
	/* flow_keys -> skb->hash is what consumers see; assert it too. */
	KUNIT_EXPECT_EQ(test, flow_hash_from_keys(&keys_slow),
			flow_hash_from_keys(&keys_fast));
}

/* Check against both eligible dissectors (different used_keys). */
static void fd_fast_check_equiv(struct kunit *test, const u8 *data,
				__be16 proto, int hlen, unsigned int flags)
{
	/* skb_get_hash() passes STOP_AT_FLOW_LABEL on every dissect; each
	 * case must hold with and without it.
	 */
	unsigned int fl = flags | FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL;

	fd_check_one(test, &flow_keys_dissector, data, proto, hlen, flags);
	fd_check_one(test, flow_keys_dissector_symmetric_kunit(),
		     data, proto, hlen, flags);
	fd_check_one(test, &flow_keys_dissector, data, proto, hlen, fl);
	fd_check_one(test, flow_keys_dissector_symmetric_kunit(),
		     data, proto, hlen, fl);
}

static void fd_fast_equiv_test(struct kunit *test)
{
	const struct fd_fast_case *c = test->param_value;
	u8 *buf;
	int len;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	len = c->build(buf);
	KUNIT_ASSERT_LE(test, len, FD_TEST_BUF_LEN);

	fd_fast_check_equiv(test, buf, c->proto, len, c->flags);
}

/* Truncation sweep: the fast path must never parse (or partially
 * write) anything the slow path rejected at the same length.
 */
static void fd_fast_truncation_test(struct kunit *test)
{
	int i, len, hlen;
	u8 *buf;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (i = 0; i < ARRAY_SIZE(fd_fast_cases); i++) {
		const struct fd_fast_case *c = &fd_fast_cases[i];

		len = c->build(buf);
		for (hlen = len - 1; hlen >= 0; hlen--)
			fd_fast_check_equiv(test, buf, c->proto, hlen,
					    c->flags);
	}
}

/* skb-mode builder (for hw-accel VLAN entry conditions). */
static struct sk_buff *fd_fast_build_skb(struct kunit *test,
					 int (*build)(u8 *buf),
					 __be16 protocol)
{
	struct sk_buff *skb;
	int len;
	u8 *buf;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	len = build(buf);

	skb = alloc_skb(FD_TEST_BUF_LEN + NET_SKB_PAD, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);
	skb_reserve(skb, NET_SKB_PAD);
	skb_put_data(skb, buf, len);
	skb->protocol = protocol;
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	return skb;
}

static void fd_check_one_skb(struct kunit *test, struct flow_dissector *fd,
			     struct sk_buff *skb)
{
	struct flow_keys keys_slow, keys_fast;
	bool ret_slow, ret_fast;

	fd_fast_gates_set(false);
	memset(&keys_slow, 0, sizeof(keys_slow));
	ret_slow = __skb_flow_dissect(&init_net, skb, fd,
				      &keys_slow, NULL, 0, 0, 0, 0);

	fd_fast_gates_set(true);
	memset(&keys_fast, 0, sizeof(keys_fast));
	ret_fast = __skb_flow_dissect(&init_net, skb, fd,
				      &keys_fast, NULL, 0, 0, 0, 0);
	fd_fast_gates_set(false);

	KUNIT_EXPECT_EQ(test, ret_slow, ret_fast);
	KUNIT_EXPECT_MEMEQ(test, &keys_slow, &keys_fast, sizeof(keys_slow));
	KUNIT_EXPECT_EQ(test, flow_hash_from_keys(&keys_slow),
			flow_hash_from_keys(&keys_fast));
}

static void fd_fast_check_equiv_skb(struct kunit *test, struct sk_buff *skb)
{
	fd_check_one_skb(test, &flow_keys_dissector, skb);
	fd_check_one_skb(test, flow_keys_dissector_symmetric_kunit(), skb);
}

static void fd_fast_skb_plain_test(struct kunit *test)
{
	struct sk_buff *skb;

	skb = fd_fast_build_skb(test, build_ipv4_tcp, htons(ETH_P_IP));
	fd_fast_check_equiv_skb(test, skb);
	kfree_skb(skb);
}

static void fd_fast_skb_hwaccel_vlan_test(struct kunit *test)
{
	struct sk_buff *skb;

	/* Plain payload; VLAN tag in skb metadata, as after hw stripping. */
	skb = fd_fast_build_skb(test, build_ipv4_tcp, htons(ETH_P_IP));
	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), 100);
	fd_fast_check_equiv_skb(test, skb);
	kfree_skb(skb);
}

static void fd_fast_skb_hwaccel_vlan_qinq_test(struct kunit *test)
{
	struct sk_buff *skb;

	/* Outer tag hw-stripped (8021AD), second tag in the payload. */
	skb = fd_fast_build_skb(test, build_vlan_ipv4_tcp, htons(ETH_P_8021Q));
	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021AD), 100);
	fd_fast_check_equiv_skb(test, skb);
	kfree_skb(skb);
}

/* Nested tunnels: both paths must stop at MAX_FLOW_DISSECT_HDRS --
 * shallow stacks descend, past the cap the fast path defers and
 * matches the slow path's capped result.
 */
static void fd_fast_deep_nest_test(struct kunit *test)
{
	int levels, len;
	u8 *buf;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (levels = 1; levels <= 24; levels++) {
		len = build_ipip_nest(buf, levels);
		if (len > FD_TEST_BUF_LEN)
			break;
		fd_fast_check_equiv(test, buf, htons(ETH_P_IP), len, 0);
	}
	for (levels = 1; levels <= 16; levels++) {
		len = build_gre_nest(buf, levels);
		if (len > FD_TEST_BUF_LEN)
			break;
		fd_fast_check_equiv(test, buf, htons(ETH_P_IP), len, 0);
	}
}

/* Non-linear skb: the fast path reads only the linear head and must
 * defer; the slow path pulls from the frag. Output must match.
 */
static void fd_fast_nonlinear_skb_test(struct kunit *test)
{
	const int linear = 8;
	struct sk_buff *skb;
	struct page *page;
	u8 *buf;
	int len;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	len = build_ipv4_tcp(buf);

	skb = alloc_skb(NET_SKB_PAD + linear, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, skb);
	skb_reserve(skb, NET_SKB_PAD);
	skb_put_data(skb, buf, linear);

	page = alloc_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, page);
	memcpy(page_address(page), buf + linear, len - linear);
	skb_add_rx_frag(skb, 0, page, 0, len - linear, len - linear);

	skb->protocol = htons(ETH_P_IP);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);

	fd_fast_check_equiv_skb(test, skb);
	kfree_skb(skb);
}

/* An ineligible (custom) dissector: the fast path must never run,
 * so gates-on output equals gates-off for the whole corpus.
 */
static void fd_fast_ineligible_dissector_test(struct kunit *test)
{
	static const struct flow_dissector_key keys[] = {
		{ .key_id = FLOW_DISSECTOR_KEY_CONTROL,
		  .offset = offsetof(struct flow_keys, control) },
		{ .key_id = FLOW_DISSECTOR_KEY_BASIC,
		  .offset = offsetof(struct flow_keys, basic) },
		{ .key_id = FLOW_DISSECTOR_KEY_IPV4_ADDRS,
		  .offset = offsetof(struct flow_keys, addrs.v4addrs) },
		{ .key_id = FLOW_DISSECTOR_KEY_IPV6_ADDRS,
		  .offset = offsetof(struct flow_keys, addrs.v6addrs) },
		{ .key_id = FLOW_DISSECTOR_KEY_PORTS,
		  .offset = offsetof(struct flow_keys, ports) },
		{ .key_id = FLOW_DISSECTOR_KEY_ICMP,
		  .offset = offsetof(struct flow_keys, icmp) },
	};
	struct flow_dissector custom;
	int i, len;
	u8 *buf;

	skb_flow_dissector_init(&custom, keys, ARRAY_SIZE(keys));

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	for (i = 0; i < ARRAY_SIZE(fd_fast_cases); i++) {
		const struct fd_fast_case *c = &fd_fast_cases[i];

		len = c->build(buf);
		fd_check_one(test, &custom, buf, c->proto, len, c->flags);
	}
}

/* skb-mode coverage for ipv6 / pppoe / mpls / gre. */
static void fd_fast_skb_shapes_test(struct kunit *test)
{
	static const struct {
		int (*build)(u8 *buf);
		__be16 proto;
	} cases[] = {
		{ build_ipv6_tcp, cpu_to_be16(ETH_P_IPV6) },
		{ build_pppoe_ipv4_tcp, cpu_to_be16(ETH_P_PPP_SES) },
		{ build_mpls_ipv4, cpu_to_be16(ETH_P_MPLS_UC) },
		{ build_gre_ipv4, cpu_to_be16(ETH_P_IP) },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct sk_buff *skb;

		skb = fd_fast_build_skb(test, cases[i].build, cases[i].proto);
		fd_fast_check_equiv_skb(test, skb);
		kfree_skb(skb);
	}
}

/* Seeded, reproducible fuzzer: mutate corpus packets and lengths,
 * assert equivalence each iteration -- the guard for divergences the
 * hand-written corpus does not name.
 */
static void fd_fast_fuzz_test(struct kunit *test)
{
	struct rnd_state rnd;
	u8 *buf;
	int i;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);
	prandom_seed_state(&rnd, 0x1a2b3c4d5e6f7788ULL);

	for (i = 0; i < FD_FUZZ_ITERS; i++) {
		const struct fd_fast_case *c;
		unsigned int flags;
		int len, j, nmut;

		c = &fd_fast_cases[prandom_u32_state(&rnd) %
				   ARRAY_SIZE(fd_fast_cases)];
		len = c->build(buf);

		/* Mutate a handful of header bytes to random values. */
		nmut = prandom_u32_state(&rnd) % (len + 1);
		for (j = 0; j < nmut; j++)
			buf[prandom_u32_state(&rnd) % len] =
				prandom_u32_state(&rnd);

		/* Random truncation and random 1st-frag flag. */
		len = prandom_u32_state(&rnd) % (len + 1);
		flags = (prandom_u32_state(&rnd) & 1) ?
			FLOW_DISSECTOR_F_PARSE_1ST_FRAG : 0;

		fd_fast_check_equiv(test, buf, c->proto, len, flags);
	}
}

/* Gates off: the fast path must never run (counters are the
 * observable). Positive controls: an eligible shape must count with
 * the gates on.
 */
static void fd_fast_gates_off_test(struct kunit *test)
{
	struct flow_keys keys;
	u64 before;
	int i, len;
	u8 *buf;

	buf = kunit_kzalloc(test, FD_TEST_BUF_LEN, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, buf);

	fd_fast_gates_set(false);
	before = flow_dissector_fast_hits_kunit();
	for (i = 0; i < ARRAY_SIZE(fd_fast_cases); i++) {
		len = fd_fast_cases[i].build(buf);
		memset(&keys, 0, sizeof(keys));
		__skb_flow_dissect(&init_net, NULL, &flow_keys_dissector,
				   &keys, buf, fd_fast_cases[i].proto, 0,
				   len, fd_fast_cases[i].flags);
	}
	KUNIT_EXPECT_EQ(test, flow_dissector_fast_hits_kunit(), before);

	fd_fast_gates_set(true);
	len = build_ipv4_tcp(buf);
	memset(&keys, 0, sizeof(keys));
	__skb_flow_dissect(&init_net, NULL, &flow_keys_dissector, &keys,
			   buf, htons(ETH_P_IP), 0, len, 0);
	KUNIT_EXPECT_GT(test, flow_dissector_fast_hits_kunit(), before);

	/* Flagship consumer shape: skb_get_hash() passes STOP_AT_FLOW_LABEL;
	 * the fast path must hit for it.
	 */
	before = flow_dissector_fast_hits_kunit();
	memset(&keys, 0, sizeof(keys));
	__skb_flow_dissect(&init_net, NULL, &flow_keys_dissector, &keys,
			   buf, htons(ETH_P_IP), 0, len,
			   FLOW_DISSECTOR_F_STOP_AT_FLOW_LABEL);
	fd_fast_gates_set(false);
	KUNIT_EXPECT_GT(test, flow_dissector_fast_hits_kunit(), before);
}

static void fd_all_gates_off(void)
{
	fd_fast_gates_set(false);
}

static int fd_fast_suite_init(struct kunit_suite *suite)
{
	fd_all_gates_off();
	return 0;
}

static void fd_fast_suite_exit(struct kunit_suite *suite)
{
	fd_all_gates_off();
}

static struct kunit_case fd_fast_test_cases[] = {
	KUNIT_CASE_PARAM(fd_fast_equiv_test, fd_fast_gen_params),
	KUNIT_CASE(fd_fast_truncation_test),
	KUNIT_CASE(fd_fast_deep_nest_test),
	KUNIT_CASE(fd_fast_nonlinear_skb_test),
	KUNIT_CASE(fd_fast_ineligible_dissector_test),
	KUNIT_CASE(fd_fast_skb_plain_test),
	KUNIT_CASE(fd_fast_skb_hwaccel_vlan_test),
	KUNIT_CASE(fd_fast_skb_hwaccel_vlan_qinq_test),
	KUNIT_CASE(fd_fast_skb_shapes_test),
	KUNIT_CASE(fd_fast_fuzz_test),
	KUNIT_CASE(fd_fast_gates_off_test),
	{}
};

static struct kunit_suite fd_fast_suite = {
	.name = "flow_dissector_fastpath",
	.suite_init = fd_fast_suite_init,
	.suite_exit = fd_fast_suite_exit,
	.test_cases = fd_fast_test_cases,
};

kunit_test_suite(fd_fast_suite);

MODULE_DESCRIPTION("KUnit fast/slow equivalence tests for the flow dissector");
MODULE_LICENSE("GPL");
