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

#include <net/route.h>

#include "address.h"
#include "icmp.h"
#include "packet.h"
#include "translate_64.h"
#include "transport.h"

#define IPXLAT_ICMP4_ERROR_MAX_LEN 576U

/* RFC 7915 Section 5.2, Figure 4 */
static const u8 ipxlat_64_icmp_param_prob_map[] = {
	0,  1,	0xff, 0xff, 2,	2,  9,	8,  12, 12, 12, 12, 12, 12,
	12, 12, 12,   12,   12, 12, 12, 12, 12, 12, 16, 16, 16, 16,
	16, 16, 16,   16,   16, 16, 16, 16, 16, 16, 16, 16,
};

static int ipxlat_64_map_icmp_param_prob(u32 ptr6, u32 *ptr4)
{
	if (unlikely(ptr6 >= ARRAY_SIZE(ipxlat_64_icmp_param_prob_map) ||
		     ipxlat_64_icmp_param_prob_map[ptr6] == 0xff))
		return -EPROTONOSUPPORT;

	*ptr4 = ipxlat_64_icmp_param_prob_map[ptr6];
	return 0;
}

static void ipxlat_icmp4_set_param_ptr(struct icmphdr *ic4, u8 ptr)
{
	ic4->un.gateway = htonl((u32)ptr << 24);
}

static int ipxlat_64_map_icmp_info_type_code(const struct icmp6hdr *in,
					     struct icmphdr *out)
{
	switch (in->icmp6_type) {
	case ICMPV6_ECHO_REQUEST:
		out->type = ICMP_ECHO;
		out->code = 0;
		out->un.echo.id = in->icmp6_identifier;
		out->un.echo.sequence = in->icmp6_sequence;
		return 0;
	case ICMPV6_ECHO_REPLY:
		out->type = ICMP_ECHOREPLY;
		out->code = 0;
		out->un.echo.id = in->icmp6_identifier;
		out->un.echo.sequence = in->icmp6_sequence;
		return 0;
	default:
		return -EINVAL;
	}
}

/* Lookup post-translation IPv4 PMTU for ICMPv6 PTB -> ICMPv4 FRAG_NEEDED.
 * Falls back to translator MTU on routing failures and clamps route MTU
 * against translator egress MTU.
 */
static unsigned int ipxlat_64_lookup_pmtu4(struct ipxlat_priv *ipxlat,
					   const struct sk_buff *skb)
{
	const struct iphdr *iph4;
	struct flowi4 fl4 = {};
	unsigned int dev_mtu;
	struct rtable *rt;
	unsigned int mtu4;

	dev_mtu = READ_ONCE(ipxlat->dev->mtu);
	iph4 = ip_hdr(skb);

	fl4.daddr = iph4->daddr;
	fl4.saddr = iph4->saddr;
	fl4.flowi4_mark = skb->mark;
	fl4.flowi4_proto = IPPROTO_ICMP;

	rt = ip_route_output_key(dev_net(ipxlat->dev), &fl4);
	if (IS_ERR(rt))
		return dev_mtu;

	/* clamp against translator MTU to avoid oversized local PMTU */
	mtu4 = min_t(unsigned int, dst_mtu(&rt->dst), dev_mtu);
	ip_rt_put(rt);

	return mtu4;
}

static int ipxlat_64_build_icmp4_errhdr(struct ipxlat_priv *ipxlat,
					struct sk_buff *skb,
					const struct icmp6hdr *ic6,
					struct icmphdr *ic4, bool *ie_forbidden)
{
	unsigned int in_mtu, out_mtu;
	u32 ptr6, ptr4;
	int err;

	switch (ic6->icmp6_type) {
	case ICMPV6_DEST_UNREACH:
		ic4->type = ICMP_DEST_UNREACH;
		switch (ic6->icmp6_code) {
		case ICMPV6_NOROUTE:
		case ICMPV6_NOT_NEIGHBOUR:
		case ICMPV6_ADDR_UNREACH:
			ic4->code = ICMP_HOST_UNREACH;
			break;
		case ICMPV6_ADM_PROHIBITED:
			ic4->code = ICMP_HOST_ANO;
			break;
		case ICMPV6_PORT_UNREACH:
			ic4->code = ICMP_PORT_UNREACH;
			break;
		default:
			return -EINVAL;
		}
		ic4->un.gateway = 0;
		*ie_forbidden = false;
		return 0;
	case ICMPV6_TIME_EXCEED:
		ic4->type = ICMP_TIME_EXCEEDED;
		ic4->code = ic6->icmp6_code;
		ic4->un.gateway = 0;
		*ie_forbidden = false;
		return 0;
	case ICMPV6_PKT_TOOBIG:
		ic4->type = ICMP_DEST_UNREACH;
		ic4->code = ICMP_FRAG_NEEDED;
		ic4->un.frag.__unused = 0;
		in_mtu = ipxlat_64_lookup_pmtu4(ipxlat, skb);
		out_mtu = READ_ONCE(ipxlat->dev->mtu);
		/* RFC 7915 Section 5.2:
		 * min((PTB_mtu - 20), mtu4_nexthop, (mtu6_nexthop - 20))
		 */
		ic4->un.frag.mtu =
			cpu_to_be16(min3(be32_to_cpu(ic6->icmp6_mtu) - 20,
					 in_mtu, out_mtu - 20));
		*ie_forbidden = true;
		return 0;
	case ICMPV6_PARAMPROB:
		ptr6 = be32_to_cpu(ic6->icmp6_dataun.un_data32[0]);
		switch (ic6->icmp6_code) {
		case ICMPV6_HDR_FIELD:
			ic4->type = ICMP_PARAMETERPROB;
			ic4->code = 0;
			err = ipxlat_64_map_icmp_param_prob(ptr6, &ptr4);
			if (unlikely(err))
				return err;
			ipxlat_icmp4_set_param_ptr(ic4, ptr4);
			break;
		case ICMPV6_UNK_NEXTHDR:
			ic4->type = ICMP_DEST_UNREACH;
			ic4->code = ICMP_PROT_UNREACH;
			ic4->un.gateway = 0;
			break;
		default:
			return -EINVAL;
		}
		*ie_forbidden = true;
		return 0;
	default:
		return -EINVAL;
	}
}

static __sum16
ipxlat_64_compute_icmp_info_csum(const struct ipv6hdr *in6,
				 const struct icmp6hdr *in_icmp6,
				 const struct icmphdr *out_icmp4,
				 unsigned int l4_len)
{
	struct icmp6hdr icmp6_zero;
	struct icmphdr icmp4_zero;
	__wsum csum, tmp;

	icmp6_zero = *in_icmp6;
	icmp6_zero.icmp6_cksum = 0;
	icmp4_zero = *out_icmp4;
	icmp4_zero.checksum = 0;

	csum = ~csum_unfold(in_icmp6->icmp6_cksum);
	tmp = ~csum_unfold(csum_ipv6_magic(&in6->saddr, &in6->daddr, l4_len,
					   NEXTHDR_ICMP, 0));
	csum = csum_sub(csum, tmp);
	csum = csum_sub(csum, csum_partial(&icmp6_zero, sizeof(icmp6_zero), 0));
	csum = csum_add(csum, csum_partial(&icmp4_zero, sizeof(icmp4_zero), 0));
	return csum_fold(csum);
}

static int ipxlat_64_icmp_info(struct sk_buff *skb, const struct ipv6hdr *in6)
{
	struct icmp6hdr ic6_copy, *ic6;
	struct icmphdr *ic4;
	int err;

	ic6 = icmp6_hdr(skb);
	ic6_copy = *ic6;

	ic4 = (struct icmphdr *)(skb->data + skb_transport_offset(skb));
	err = ipxlat_64_map_icmp_info_type_code(&ic6_copy, ic4);
	if (unlikely(err))
		return err;

	ic4->checksum =
		ipxlat_64_compute_icmp_info_csum(in6, &ic6_copy, ic4,
						 ipxlat_skb_datagram_len(skb));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

static int ipxlat_64_icmp_inner_info(struct sk_buff *skb,
				     unsigned int inner_l4_off)
{
	struct icmphdr *ic4;
	struct icmp6hdr ic6;
	int err;

	/* inner header alignment is not guaranteed */
	memcpy(&ic6, skb->data + inner_l4_off, sizeof(ic6));
	ic4 = (struct icmphdr *)(skb->data + inner_l4_off);
	err = ipxlat_64_map_icmp_info_type_code(&ic6, ic4);
	if (unlikely(err))
		return err;

	ic4->checksum = 0;
	ic4->checksum = csum_fold(skb_checksum(skb, inner_l4_off,
					       skb->len - inner_l4_off, 0));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

static int ipxlat_64_icmp_inner_l4(struct sk_buff *skb,
				   unsigned int inner_l4_off,
				   const struct iphdr *inner4,
				   const struct ipv6hdr *inner6)
{
	struct tcphdr *tcp;
	struct udphdr *udp;

	switch (inner4->protocol) {
	case IPPROTO_TCP:
		tcp = (struct tcphdr *)(skb->data + inner_l4_off);
		return ipxlat_64_inner_tcp(skb, inner6, inner4, tcp);
	case IPPROTO_UDP:
		udp = (struct udphdr *)(skb->data + inner_l4_off);
		return ipxlat_64_inner_udp(skb, inner6, inner4, udp);
	case IPPROTO_ICMP:
		return ipxlat_64_icmp_inner_info(skb, inner_l4_off);
	default:
		return 0;
	}
}

static int ipxlat_64_icmp_inner(struct ipxlat_priv *ipxlat, struct sk_buff *skb,
				int *inner_delta)
{
	unsigned int old_prefix, new_prefix, inner_l3_len, inner_tot_len,
		inner_l4_payload, outer_prefix, inner_l3_off, inner_l4_old_off;
	const unsigned int outer_l3_len = skb_transport_offset(skb);
	const struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct iphdr outer4_copy = *ip_hdr(skb);
	bool has_inner_frag, first_inner_frag, mf, df;
	struct frag_hdr inner_fragh;
	struct ipv6hdr inner6;
	struct iphdr *inner4;
	__be32 saddr, daddr;
	u16 frag_off;
	u8 inner_l4_proto;
	__be16 frag_id;
	int err;

	inner_l3_off = cb->inner_l3_offset;
	inner_l4_old_off = cb->inner_l4_offset;
	inner_l3_len = inner_l4_old_off - inner_l3_off;
	outer_prefix = inner_l3_off;

	inner_l4_proto = ipxlat_64_map_nexthdr_proto(cb->inner_l4_proto);
	has_inner_frag = !!cb->inner_fragh_off;

	/* inner header alignment is not guaranteed */
	memcpy(&inner6, skb->data + outer_prefix, sizeof(inner6));

	first_inner_frag = true;
	if (unlikely(has_inner_frag)) {
		memcpy(&inner_fragh, skb->data + cb->inner_fragh_off,
		       sizeof(inner_fragh));
		first_inner_frag = ipxlat_is_first_frag6(&inner_fragh);
	}

	err = ipxlat_64_convert_addrs(&ipxlat->xlat_prefix6, &inner6, false,
				      &saddr, &daddr);
	if (unlikely(err))
		return err;

	old_prefix = outer_prefix + inner_l3_len;
	new_prefix = outer_prefix + sizeof(struct iphdr);
	*inner_delta = (int)new_prefix - (int)old_prefix;

	/* unlike 46, inner 6->4 always shrinks quoted L3 size */
	skb_pull(skb, old_prefix);
	skb_push(skb, new_prefix);
	/* outer 6->4 translation already set network/transport headers, but
	 * inner relayout pulls/pushes again and changes skb->data placement.
	 * Reinitialize outer header offsets so ip{,v6}_hdr/icmp{,6}_hdr and
	 * skb_transport_offset keep pointing to the outer packet.
	 */
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, outer_l3_len);

	*ip_hdr(skb) = outer4_copy;

	inner4 = (struct iphdr *)(skb->data + outer_prefix);
	inner_tot_len = ntohs(inner6.payload_len) + sizeof(inner6) -
			inner_l3_len + sizeof(struct iphdr);
	/* RFC 7915 Section 5.1 */
	if (likely(!has_inner_frag)) {
		df = inner_tot_len > (IPV6_MIN_MTU - sizeof(struct iphdr));
		inner4->frag_off = ipxlat_build_frag4_offset(df, false, 0);
	} else {
		mf = !!(be16_to_cpu(inner_fragh.frag_off) & IP6_MF);
		frag_off = ipxlat_get_frag6_offset(&inner_fragh);
		inner4->frag_off =
			ipxlat_build_frag4_offset(false, mf, frag_off);
	}

	/* keep low 16 bits of IPv6 Fragment ID as numeric value, then re-encode
	 * to network-order IPv4 ID
	 */
	frag_id = has_inner_frag ?
			  cpu_to_be16(be32_to_cpu(inner_fragh.identification)) :
			  0;
	ipxlat_64_build_l3(inner4, &inner6, inner_tot_len, inner4->frag_off,
			   inner_l4_proto, saddr, daddr, inner6.hop_limit,
			   frag_id);

	if (likely(!has_inner_frag)) {
		inner4->id = 0;
		__ip_select_ident(dev_net(ipxlat->dev), inner4, 1);
		inner4->check = 0;
		inner4->check = ip_fast_csum(inner4, inner4->ihl);
	}

	if (unlikely(!first_inner_frag))
		return 0;

	inner_l4_payload = new_prefix + ipxlat_l4_min_len(inner4->protocol);
	if (unlikely(skb_ensure_writable(skb, inner_l4_payload)))
		return -ENOMEM;

	return ipxlat_64_icmp_inner_l4(skb, new_prefix, inner4, &inner6);
}

/* Rebuild ICMPv4 quoted-datagram/extensions after inner 6->4 translation.
 *
 * The inner rewrite changes the quoted datagram length. This helper updates
 * the RFC 4884 delimiter/padding and extension bytes, then enforces the
 * IPv4 ICMP error size cap.
 *
 * This is intentionally not a mirror of ipxlat_46_icmp_squeeze_ext:
 * - 4->6 always writes icmp6_datagram_len (either computed or 0).
 * - 6->4 updates ICMPv4 datagram-length only when extensions are allowed.
 *   Some mapped ICMPv6 errors set ie_forbidden, and in that case we keep the
 *   ICMPv4 header semantics for that type/code and only relayout/trim payload.
 */
static int ipxlat_64_squeeze_icmp_ext(struct sk_buff *skb,
				      unsigned int icmp6_ipl, int inner_delta,
				      bool ie_forbidden)
{
	unsigned int outer_hdrs_len, payload_len, icmp4_iel_in, icmp4_iel_out;
	unsigned int out_pad, max_iel, pkt_len_cap, icmp4_ipl_out_bytes;
	unsigned int icmp4_ipl_out = 0, icmp4_ipl_in_bytes;
	unsigned int new_tot_len;
	int icmp4_ipl_in, err;
	struct icmphdr *ic4;
	struct iphdr *iph4;

	if (likely(!icmp6_ipl))
		goto finalize;

	outer_hdrs_len = skb_transport_offset(skb) + sizeof(struct icmphdr);
	if (unlikely(skb->len < outer_hdrs_len))
		return -EINVAL;

	payload_len = skb->len - outer_hdrs_len;
	icmp4_ipl_in = (int)icmp6_ipl + inner_delta;
	if (unlikely(icmp4_ipl_in < 0))
		return -EINVAL;
	icmp4_ipl_in_bytes = icmp4_ipl_in;
	if (unlikely(icmp4_ipl_in_bytes > payload_len))
		return -EINVAL;

	if (likely(icmp4_ipl_in_bytes == payload_len))
		goto finalize;

	icmp4_iel_in = payload_len - icmp4_ipl_in_bytes;
	max_iel = IPXLAT_ICMP4_ERROR_MAX_LEN -
		  (outer_hdrs_len + ICMP_EXT_ORIG_DGRAM_MIN_LEN);

	if (unlikely(ie_forbidden)) {
		icmp4_ipl_out_bytes = icmp4_ipl_in_bytes;
		out_pad = 0;
		icmp4_iel_out = 0;
	} else if (unlikely(icmp4_iel_in > max_iel)) {
		pkt_len_cap = min_t(unsigned int, skb->len - icmp4_iel_in,
				    IPXLAT_ICMP4_ERROR_MAX_LEN);
		icmp4_ipl_out_bytes = pkt_len_cap - outer_hdrs_len;
		out_pad = 0;
		icmp4_iel_out = 0;
		icmp4_ipl_out = 0;
	} else {
		pkt_len_cap = min_t(unsigned int, skb->len,
				    IPXLAT_ICMP4_ERROR_MAX_LEN);
		icmp4_ipl_out_bytes =
			round_down(pkt_len_cap - icmp4_iel_in - outer_hdrs_len,
				   sizeof(u32));
		out_pad = max_t(unsigned int, ICMP_EXT_ORIG_DGRAM_MIN_LEN,
				icmp4_ipl_out_bytes) -
			  icmp4_ipl_out_bytes;
		icmp4_iel_out = icmp4_iel_in;
		/* RFC 4884 field is in 32-bit units for ICMPv4 errors */
		icmp4_ipl_out = (icmp4_ipl_out_bytes + out_pad) >> 2;
	}

	/* if no extension bytes are copied and no pad is written, relayout only
	 * trims/updates lengths and does not require full data writability
	 */
	if (unlikely(icmp4_iel_out || out_pad)) {
		err = skb_ensure_writable(skb, skb->len);
		if (unlikely(err))
			return err;
	}

	err = ipxlat_icmp_relayout(skb, outer_hdrs_len, icmp4_ipl_in_bytes,
				   icmp4_iel_in, icmp4_ipl_out_bytes, out_pad,
				   icmp4_iel_out);
	if (unlikely(err))
		return err;

finalize:
	if (!ie_forbidden) {
		ic4 = icmp_hdr(skb);
		ic4->un.reserved[1] = icmp4_ipl_out;
	}

	if (unlikely(skb->len > IPXLAT_ICMP4_ERROR_MAX_LEN)) {
		err = pskb_trim(skb, IPXLAT_ICMP4_ERROR_MAX_LEN);
		if (unlikely(err))
			return err;
	}

	iph4 = ip_hdr(skb);
	new_tot_len = skb->len;
	if (unlikely(be16_to_cpu(iph4->tot_len) != new_tot_len)) {
		iph4->tot_len = cpu_to_be16(new_tot_len);
		/* relayout/trim may invalidate precomputed DF decision */
		iph4->frag_off &= cpu_to_be16(~IP_DF);
		iph4->check = 0;
		iph4->check = ip_fast_csum(iph4, iph4->ihl);
	}

	return 0;
}

/**
 * ipxlat_64_icmp_error - translate ICMPv6 error payload to ICMPv4 error form
 * @ipxlat: translator private context
 * @skb: packet carrying outer ICMPv6 error
 *
 * Rewrites the quoted inner datagram in place, maps type/code/fields and
 * adjusts RFC 4884 datagram/extension layout before recomputing outer checksum.
 *
 * Return: 0 on success, negative errno on translation failure.
 */
static int ipxlat_64_icmp_error(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	const struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct icmp6hdr ic6 = *icmp6_hdr(skb);
	unsigned int icmp6_ipl;
	int inner_delta, err;
	struct icmphdr *ic4;
	bool ie_forbidden;

	if (unlikely(!(cb->is_icmp_err))) {
		DEBUG_NET_WARN_ON_ONCE(1);
		return -EINVAL;
	}

	/* translate quoted inner packet headers */
	err = ipxlat_64_icmp_inner(ipxlat, skb, &inner_delta);
	if (unlikely(err))
		return err;

	/* build outer ICMPv4 error header after inner relayout */
	ic4 = (struct icmphdr *)(skb->data + skb_transport_offset(skb));
	err = ipxlat_64_build_icmp4_errhdr(ipxlat, skb, &ic6, ic4,
					   &ie_forbidden);
	if (unlikely(err))
		return err;

	icmp6_ipl = ic6.icmp6_datagram_len << 3;
	err = ipxlat_64_squeeze_icmp_ext(skb, icmp6_ipl, inner_delta,
					 ie_forbidden);
	if (unlikely(err))
		return err;

	/* recompute whole ICMPv4 checksum after error-path relayout */
	ic4->checksum = 0;
	ic4->checksum = csum_fold(skb_checksum(skb, skb_transport_offset(skb),
					       ipxlat_skb_datagram_len(skb),
					       0));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_64_icmp(struct ipxlat_priv *ipxlat, struct sk_buff *skb,
		   const struct ipv6hdr *in6)
{
	if (unlikely(ipxlat_skb_cb(skb)->is_icmp_err))
		return ipxlat_64_icmp_error(ipxlat, skb);

	return ipxlat_64_icmp_info(skb, in6);
}
