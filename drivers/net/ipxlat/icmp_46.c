// SPDX-License-Identifier: GPL-2.0
/*  IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
 *
 *  Copyright (C) 2024- Alberto Leiva Popper <ydahhrk@gmail.com>
 *  Copyright (C) 2026- Mandelbit SRL
 *  Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
 *
 *  Author:	Alberto Leiva Popper <ydahhrk@gmail.com>
 *		Antonio Quartulli <antonio@mandelbit.com>
 *		Daniel Gröber <dxld@darkboxed.org>
 *		Ralf Lici <ralf@mandelbit.com>
 */

#include "address.h"
#include "icmp.h"
#include "packet.h"
#include "translate_46.h"
#include "transport.h"

#define IPXLAT_ICMP4_PP_CODE_PTR 0
#define IPXLAT_ICMP4_PP_CODE_BADLEN 2

/* RFC 7915 Section 4.2, Figure 3 */
static const u8 ipxlat_46_icmp_param_prob_map[] = { 0,	1,    4,    4,	0xff,
						  0xff, 0xff, 0xff, 7,	6,
						  0xff, 0xff, 8,    8,	8,
						  8,	24,   24,   24, 24 };

/* RFC 1191 plateau table used when ICMPv4 FRAG_NEEDED reports MTU=0 */
static const u16 ipxlat_46_mtu_plateaus[] = {
	65535, 32000, 17914, 8166, 4352, 2002, 1492,
};

static u8 ipxlat_icmp4_get_param_ptr(const struct icmphdr *ic4)
{
	return ntohl(ic4->un.gateway) >> 24;
}

static int ipxlat_46_map_icmp_param_prob(const struct icmphdr *in,
					 struct icmp6hdr *out)
{
	u8 ptr;

	if (unlikely(in->code != IPXLAT_ICMP4_PP_CODE_PTR &&
		     in->code != IPXLAT_ICMP4_PP_CODE_BADLEN))
		return -EPROTONOSUPPORT;

	ptr = ipxlat_icmp4_get_param_ptr(in);
	if (unlikely(ptr >= ARRAY_SIZE(ipxlat_46_icmp_param_prob_map) ||
		     ipxlat_46_icmp_param_prob_map[ptr] == 0xff))
		return -EPROTONOSUPPORT;

	out->icmp6_pointer = cpu_to_be32(ipxlat_46_icmp_param_prob_map[ptr]);
	return 0;
}

static int ipxlat_46_map_icmp_info_type_code(const struct icmphdr *in,
					     struct icmp6hdr *out)
{
	switch (in->type) {
	case ICMP_ECHO:
		out->icmp6_type = ICMPV6_ECHO_REQUEST;
		out->icmp6_code = 0;
		out->icmp6_identifier = in->un.echo.id;
		out->icmp6_sequence = in->un.echo.sequence;
		return 0;
	case ICMP_ECHOREPLY:
		out->icmp6_type = ICMPV6_ECHO_REPLY;
		out->icmp6_code = 0;
		out->icmp6_identifier = in->un.echo.id;
		out->icmp6_sequence = in->un.echo.sequence;
		return 0;
	}

	return -EPROTONOSUPPORT;
}

static __be32 ipxlat_46_compute_icmp_mtu6(unsigned int pkt_mtu,
					  unsigned int nexthop6mtu,
					  unsigned int nexthop4mtu,
					  u16 tot_len_field)
{
	unsigned int i;
	u32 result;

	/* RFC 7915 Section 4.2:
	 * If the IPv4 router set the MTU field to zero, then the translator
	 * MUST use the plateau values specified in RFC 1191 to determine a
	 * likely path MTU and include that path MTU in the ICMPv6 packet.
	 */
	if (unlikely(pkt_mtu == 0)) {
		for (i = 0; i < ARRAY_SIZE(ipxlat_46_mtu_plateaus); i++) {
			if (ipxlat_46_mtu_plateaus[i] < tot_len_field) {
				pkt_mtu = ipxlat_46_mtu_plateaus[i];
				break;
			}
		}
	}

	/* RFC 7915 Section 4.2:
	 * max(1280, min(pkt_mtu + 20, mtu6_nexthop, mtu4_nexthop + 20))
	 *
	 * pkt_mtu + 20 converts ICMPv4-reported MTU to IPv6 context.
	 * mtu6_nexthop and mtu4_nexthop + 20 clamp to local next-hop limits.
	 * max(..., 1280) enforces IPv6 minimum MTU.
	 */
	result = min(pkt_mtu + 20, min(nexthop6mtu, nexthop4mtu + 20));
	if (result < IPV6_MIN_MTU)
		result = IPV6_MIN_MTU;

	return cpu_to_be32(result);
}

static int ipxlat_46_build_icmp_dest_unreach(struct ipxlat_priv *ipxlat,
					     struct sk_buff *skb,
					     const struct icmphdr *in,
					     struct icmp6hdr *out,
					     const struct iphdr *inner4)
{
	unsigned int inner4_tot_len, in_frag_mtu, in_mtu, out_mtu;

	switch (in->code) {
	case ICMP_NET_UNREACH:
	case ICMP_HOST_UNREACH:
	case ICMP_SR_FAILED:
	case ICMP_NET_UNKNOWN:
	case ICMP_HOST_UNKNOWN:
	case ICMP_HOST_ISOLATED:
	case ICMP_NET_UNR_TOS:
	case ICMP_HOST_UNR_TOS:
	case ICMP_PORT_UNREACH:
	case ICMP_NET_ANO:
	case ICMP_HOST_ANO:
	case ICMP_PKT_FILTERED:
	case ICMP_PREC_CUTOFF:
		out->icmp6_unused = 0;
		return 0;
	case ICMP_PROT_UNREACH:
		out->icmp6_pointer =
			cpu_to_be32(offsetof(struct ipv6hdr, nexthdr));
		return 0;
	case ICMP_FRAG_NEEDED:
		in_frag_mtu = be16_to_cpu(in->un.frag.mtu);
		inner4_tot_len = be16_to_cpu(inner4->tot_len);
		in_mtu = READ_ONCE(ipxlat->dev->mtu);
		out_mtu = ipxlat_46_lookup_pmtu6(ipxlat, skb, inner4);

		out->icmp6_mtu =
			ipxlat_46_compute_icmp_mtu6(in_frag_mtu, out_mtu,
						    in_mtu, inner4_tot_len);
		return 0;
	}

	return -EPROTONOSUPPORT;
}

static int ipxlat_46_map_icmp_type_code(struct ipxlat_priv *ipxlat,
					struct sk_buff *skb,
					const struct icmphdr *in,
					struct icmp6hdr *out,
					const struct iphdr *inner4,
					bool *ie_forbidden)
{
	int err;

	*ie_forbidden = false;

	switch (in->type) {
	case ICMP_ECHO:
	case ICMP_ECHOREPLY:
		return ipxlat_46_map_icmp_info_type_code(in, out);
	case ICMP_DEST_UNREACH:
		switch (in->code) {
		case ICMP_NET_UNREACH:
		case ICMP_HOST_UNREACH:
		case ICMP_SR_FAILED:
		case ICMP_NET_UNKNOWN:
		case ICMP_HOST_UNKNOWN:
		case ICMP_HOST_ISOLATED:
		case ICMP_NET_UNR_TOS:
		case ICMP_HOST_UNR_TOS:
			out->icmp6_type = ICMPV6_DEST_UNREACH;
			out->icmp6_code = ICMPV6_NOROUTE;
			break;
		case ICMP_PROT_UNREACH:
			out->icmp6_type = ICMPV6_PARAMPROB;
			out->icmp6_code = ICMPV6_UNK_NEXTHDR;
			*ie_forbidden = true;
			break;
		case ICMP_PORT_UNREACH:
			out->icmp6_type = ICMPV6_DEST_UNREACH;
			out->icmp6_code = ICMPV6_PORT_UNREACH;
			break;
		case ICMP_FRAG_NEEDED:
			out->icmp6_type = ICMPV6_PKT_TOOBIG;
			out->icmp6_code = 0;
			*ie_forbidden = true;
			break;
		case ICMP_NET_ANO:
		case ICMP_HOST_ANO:
		case ICMP_PKT_FILTERED:
		case ICMP_PREC_CUTOFF:
			out->icmp6_type = ICMPV6_DEST_UNREACH;
			out->icmp6_code = ICMPV6_ADM_PROHIBITED;
			break;
		default:
			return -EPROTONOSUPPORT;
		}
			return ipxlat_46_build_icmp_dest_unreach(ipxlat,
								 skb, in, out,
								 inner4);
	case ICMP_TIME_EXCEEDED:
		out->icmp6_type = ICMPV6_TIME_EXCEED;
		out->icmp6_code = in->code;
		out->icmp6_unused = 0;
		return 0;
	case ICMP_PARAMETERPROB:
		out->icmp6_type = ICMPV6_PARAMPROB;
		*ie_forbidden = true;
		switch (in->code) {
		case IPXLAT_ICMP4_PP_CODE_PTR:
		case IPXLAT_ICMP4_PP_CODE_BADLEN:
			out->icmp6_code = ICMPV6_HDR_FIELD;
			break;
		default:
			return -EPROTONOSUPPORT;
		}
		err = ipxlat_46_map_icmp_param_prob(in, out);
		if (unlikely(err))
			return err;
		return 0;
	}

	return -EPROTONOSUPPORT;
}

static void ipxlat_46_icmp_info_update_csum(const struct icmphdr *icmp4,
					    struct icmp6hdr *icmp6,
					    const struct ipv6hdr *ip6,
					    const struct sk_buff *skb,
					    unsigned int l4_off)
{
	struct icmp6hdr icmp6_zero;
	struct icmphdr icmp4_zero;
	__wsum csum;

	icmp4_zero = *icmp4;
	icmp4_zero.checksum = 0;
	icmp6_zero = *icmp6;
	icmp6_zero.icmp6_cksum = 0;
	csum = ~csum_unfold(icmp4->checksum);
	csum = csum_sub(csum, csum_partial(&icmp4_zero, sizeof(icmp4_zero), 0));
	csum = csum_add(csum, csum_partial(&icmp6_zero, sizeof(icmp6_zero), 0));
	icmp6->icmp6_cksum = csum_ipv6_magic(&ip6->saddr, &ip6->daddr,
					     skb->len - l4_off,
					     IPPROTO_ICMPV6, csum);
}

static int ipxlat_46_icmp_info_outer(struct sk_buff *skb)
{
	const unsigned int l4_off = skb_transport_offset(skb);
	const struct icmphdr icmp4 = *icmp_hdr(skb);
	const struct ipv6hdr *ip6 = ipv6_hdr(skb);
	struct icmp6hdr *icmp6 = icmp6_hdr(skb);
	int err;

	err = ipxlat_46_map_icmp_info_type_code(&icmp4, icmp6);
	if (unlikely(err))
		return -EINVAL;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		icmp6->icmp6_cksum = ~csum_ipv6_magic(&ip6->saddr, &ip6->daddr,
						      skb->len - l4_off,
						      IPPROTO_ICMPV6, 0);
		return ipxlat_set_partial_csum(skb, offsetof(struct icmp6hdr,
							   icmp6_cksum));
	}

	ipxlat_46_icmp_info_update_csum(&icmp4, icmp6, ip6, skb, l4_off);
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

static int ipxlat_46_icmp_info_inner(struct sk_buff *skb,
				     unsigned int inner_l4_off,
				     const struct ipv6hdr *inner6)
{
	struct icmp6hdr *icmp6;
	struct icmphdr icmp4;
	int err;

	/* inner header alignment is not guaranteed */
	memcpy(&icmp4, skb->data + inner_l4_off, sizeof(icmp4));
	icmp6 = (struct icmp6hdr *)(skb->data + inner_l4_off);

	err = ipxlat_46_map_icmp_info_type_code(&icmp4, icmp6);
	if (unlikely(err))
		return -EINVAL;

	ipxlat_46_icmp_info_update_csum(&icmp4, icmp6, inner6, skb,
					inner_l4_off);
	return 0;
}

static int ipxlat_46_icmp_inner_l4(struct sk_buff *skb,
				   unsigned int inner_l4_off,
				   const struct iphdr *inner4,
				   const struct ipv6hdr *inner6)
{
	struct tcphdr *tcp;
	struct udphdr *udp;

	switch (inner4->protocol) {
	case IPPROTO_TCP:
		tcp = (struct tcphdr *)(skb->data + inner_l4_off);
		return ipxlat_46_inner_tcp(skb, inner4, inner6, tcp);
	case IPPROTO_UDP:
		udp = (struct udphdr *)(skb->data + inner_l4_off);
		return ipxlat_46_inner_udp(skb, inner4, inner6, udp);
	case IPPROTO_ICMP:
		return ipxlat_46_icmp_info_inner(skb, inner_l4_off, inner6);
	default:
		return 0;
	}
}

static int ipxlat_46_icmp_inner(struct ipxlat_priv *ipxlat,
				struct sk_buff *skb, struct iphdr *inner4,
				int *inner_delta)
{
	unsigned int inner_l3_len, inner_l3_off, inner_l4_off, old_prefix,
		new_prefix, inner_tot_len, inner_l3_payload, inner_l4_payload;
	const unsigned int outer_l3_len = skb_transport_offset(skb);
	const struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	struct ipv6hdr outer_ip6_copy, *inner_ip6;
	struct frag_hdr *fh6;
	u8 next_hdr;
	bool has_inner_frag;

	inner_l3_off = cb->inner_l3_offset;
	inner_l4_off = cb->inner_l4_offset;

	/* inner header alignment is not guaranteed */
	memcpy(inner4, skb->data + inner_l3_off, sizeof(*inner4));
	inner_l3_len = inner4->ihl << 2;
	has_inner_frag = ip_is_fragment(inner4);

	/* save outer IPv6 hdr because pull+push destroys that hdr region */
	outer_ip6_copy = *ipv6_hdr(skb);

	old_prefix = inner_l3_off + inner_l3_len;
	new_prefix = inner_l3_off + sizeof(struct ipv6hdr) +
		     (has_inner_frag ? sizeof(struct frag_hdr) : 0);
	*inner_delta = (int)new_prefix - (int)old_prefix;

	if (unlikely(skb_cow_head(skb, max_t(int, 0, *inner_delta))))
		return -ENOMEM;

	skb_pull(skb, old_prefix);
	skb_push(skb, new_prefix);
	/* outer 4->6 path already set header offsets, but inner relayout
	 * pulls/pushes change skb->data placement. Reinitialize outer header
	 * offsets so ip{,v6}_hdr/icmp{,6}_hdr and skb_transport_offset keep
	 * pointing to the outer packet.
	 */
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, outer_l3_len);

	*ipv6_hdr(skb) = outer_ip6_copy;
	ipv6_hdr(skb)->payload_len = htons(skb->len - sizeof(struct ipv6hdr));

	inner_ip6 = (struct ipv6hdr *)(skb->data + inner_l3_off);
	/* use quoted IPv4 total-length, not skb->len:
	 * skb->len also includes ICMP extension bytes at the end, which are
	 * not part of the quoted inner IP datagram length.
	 */
	inner_tot_len = ntohs(inner4->tot_len);
	if (unlikely(inner_tot_len < inner_l3_len))
		return -EINVAL;

	inner_l3_payload = inner_tot_len - inner_l3_len +
			   (has_inner_frag ? sizeof(struct frag_hdr) : 0);
	if (has_inner_frag)
		next_hdr = NEXTHDR_FRAGMENT;
	else
		next_hdr = ipxlat_46_map_proto_to_nexthdr(inner4->protocol);

	ipxlat_46_build_l3(inner_ip6, inner4, inner_l3_payload, next_hdr,
			   inner4->ttl);

	ipxlat_46_convert_addrs(&ipxlat->xlat_prefix6, inner4, inner_ip6);

	if (unlikely(has_inner_frag)) {
		fh6 = (struct frag_hdr *)(inner_ip6 + 1);
		ipxlat_46_build_frag_hdr(fh6, inner4, inner4->protocol);
	}

	if (unlikely(!ipxlat_is_first_frag4(inner4)))
		return 0;

	inner_l4_payload = new_prefix + ipxlat_l4_min_len(inner4->protocol);
	if (unlikely(skb_ensure_writable(skb, inner_l4_payload)))
		return -ENOMEM;

	return ipxlat_46_icmp_inner_l4(skb, new_prefix, inner4, inner_ip6);
}

/* Adjust ICMP error quoted-datagram/extensions after inner 4->6 translation.
 * The inner rewrite changes quoted datagram length; this helper recomputes
 * RFC 4884 delimiter/padding, preserves extensions only when allowed, and
 * enforces IPv6 minimum-MTU packet size constraints.
 */
static int ipxlat_46_icmp_squeeze_ext(struct sk_buff *skb,
				      unsigned int icmp4_ipl, int inner_delta,
				      bool ie_forbidden)
{
	unsigned int icmp6_iel_in, icmp6_iel_out, max_iel, outer_hdrs_len,
		out_pad, payload_len, icmp6_ipl_out_bytes, pkt_len_cap;
	unsigned int icmp6_ipl_out = 0;
	int icmp6_ipl_in_bytes, err;
	struct icmp6hdr *ic6;
	struct ipv6hdr *iph6;

	/* icmp4_ipl marks where quoted datagram ends and extension area starts
	 */
	if (likely(!icmp4_ipl))
		goto no_extensions;

	outer_hdrs_len = skb_transport_offset(skb) + sizeof(struct icmp6hdr);
	payload_len = skb->len - outer_hdrs_len;
	icmp6_ipl_in_bytes = icmp4_ipl + inner_delta;
	if (unlikely(icmp6_ipl_in_bytes < 0 ||
		     icmp6_ipl_in_bytes > payload_len))
		return -EINVAL;

	if (likely(icmp6_ipl_in_bytes == payload_len))
		goto no_extensions;

	icmp6_iel_in = payload_len - icmp6_ipl_in_bytes;
	max_iel = IPV6_MIN_MTU - (outer_hdrs_len + ICMP_EXT_ORIG_DGRAM_MIN_LEN);

	if (unlikely(ie_forbidden || icmp6_iel_in > max_iel)) {
		pkt_len_cap = min_t(unsigned int, skb->len - icmp6_iel_in,
				    IPV6_MIN_MTU);
		icmp6_ipl_out_bytes = pkt_len_cap - outer_hdrs_len;
		out_pad = 0;
		icmp6_iel_out = 0;
		icmp6_ipl_out = 0;
	} else {
		pkt_len_cap = min_t(unsigned int, skb->len, IPV6_MIN_MTU);
		icmp6_ipl_out_bytes =
			round_down(pkt_len_cap - icmp6_iel_in - outer_hdrs_len,
				   sizeof(u64));
		out_pad = max_t(unsigned int, ICMP_EXT_ORIG_DGRAM_MIN_LEN,
				icmp6_ipl_out_bytes) -
			  icmp6_ipl_out_bytes;
		icmp6_iel_out = icmp6_iel_in;
		icmp6_ipl_out = (icmp6_ipl_out_bytes + out_pad) >> 3;
	}

	/* if no extension bytes are copied and no pad is written, relayout only
	 * trims/updates lengths and does not require full data writability
	 */
	if (unlikely(icmp6_iel_out || out_pad)) {
		err = skb_ensure_writable(skb, skb->len);
		if (unlikely(err))
			return err;
	}

	err = ipxlat_icmp_relayout(skb, outer_hdrs_len, icmp6_ipl_in_bytes,
				   icmp6_iel_in, icmp6_ipl_out_bytes, out_pad,
				   icmp6_iel_out);
	if (unlikely(err))
		return err;

	iph6 = ipv6_hdr(skb);
	iph6->payload_len = htons(skb->len - sizeof(*iph6));

no_extensions:
	if (unlikely(skb->len > IPV6_MIN_MTU)) {
		err = pskb_trim(skb, IPV6_MIN_MTU);
		if (unlikely(err))
			return err;

		iph6 = ipv6_hdr(skb);
		iph6->payload_len = htons(skb->len - sizeof(*iph6));
	}

	ic6 = icmp6_hdr(skb);
	ic6->icmp6_datagram_len = icmp6_ipl_out;
	return 0;
}

/**
 * ipxlat_46_icmp_error - translate ICMPv4 error payload to ICMPv6 error form
 * @ipxlat: translator private context
 * @skb: packet carrying outer ICMPv4 error
 *
 * Rewrites the quoted inner datagram in place, maps type/code/fields and
 * adjusts RFC 4884 datagram/extension layout before recomputing outer checksum.
 *
 * Return: 0 on success, negative errno on translation failure.
 */
static int ipxlat_46_icmp_error(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	const struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct icmphdr icmp4 = *icmp_hdr(skb);
	struct iphdr inner4_ip;
	int inner_delta, err;
	bool ie_forbidden;

	if (unlikely(!(cb->is_icmp_err))) {
		DEBUG_NET_WARN_ON_ONCE(1);
		return -EINVAL;
	}

	/* translate quoted inner packet headers */
	err = ipxlat_46_icmp_inner(ipxlat, skb, &inner4_ip, &inner_delta);
	if (unlikely(err))
		return err;

	err = ipxlat_46_map_icmp_type_code(ipxlat, skb, &icmp4, icmp6_hdr(skb),
					   &inner4_ip, &ie_forbidden);
	if (unlikely(err))
		return err;

	err = ipxlat_46_icmp_squeeze_ext(skb, icmp4.un.reserved[1] << 2,
					 inner_delta, ie_forbidden);
	if (unlikely(err))
		return err;

	/* error path rewrites quoted packet bytes/lengths, so use full
	 * checksum recomputation instead of incremental update
	 */
	icmp6_hdr(skb)->icmp6_cksum = 0;
	icmp6_hdr(skb)->icmp6_cksum =
		ipxlat_l4_csum_ipv6(&ipv6_hdr(skb)->saddr,
				    &ipv6_hdr(skb)->daddr, skb,
				    skb_transport_offset(skb),
				    ipxlat_skb_datagram_len(skb),
				    IPPROTO_ICMPV6);
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_46_icmp(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	if (unlikely(ipxlat_skb_cb(skb)->is_icmp_err))
		return ipxlat_46_icmp_error(ipxlat, skb);

	return ipxlat_46_icmp_info_outer(skb);
}
