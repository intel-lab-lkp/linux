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

#include <linux/icmp.h>

#include "dispatch.h"
#include "packet.h"

/* Shift cached skb cb offsets by the L3 header delta after in-place rewrite.
 *
 * Translation may replace only the outer L3 header size (4->6 or 6->4), while
 * cached offsets were computed before rewrite. Rebasing applies the same delta
 * to all cached absolute offsets so they still point to the same logical
 * fields in the modified skb.
 *
 * This helper only guards against underflow (< 0). Relative ordering checks
 * are done by ipxlat_cb_offsets_valid.
 */
int ipxlat_cb_rebase_offsets(struct ipxlat_cb *cb, int delta)
{
	int off;

	off = cb->l4_off + delta;
	if (unlikely(off < 0))
		return -EINVAL;
	cb->l4_off = off;

	off = cb->payload_off + delta;
	if (unlikely(off < 0))
		return -EINVAL;
	cb->payload_off = off;

	if (unlikely(cb->is_icmp_err)) {
		off = cb->inner_l3_offset + delta;
		if (unlikely(off < 0))
			return -EINVAL;
		cb->inner_l3_offset = off;

		off = cb->inner_l4_offset + delta;
		if (unlikely(off < 0))
			return -EINVAL;
		cb->inner_l4_offset = off;

		if (cb->inner_fragh_off) {
			off = cb->inner_fragh_off + delta;
			if (unlikely(off < 0))
				return -EINVAL;
			cb->inner_fragh_off = off;
		}
	}

	return 0;
}

#ifdef CONFIG_DEBUG_NET
/* Verify ordering/range relations between cached skb cb offsets.
 *
 * Unlike ipxlat_cb_rebase_offsets, this checks structural invariants:
 * l4 <= payload, inner_l3 >= payload, inner_l3 <= inner_l4, and fragment
 * header (when present) located inside inner L3 area before inner L4.
 */
bool ipxlat_cb_offsets_valid(const struct ipxlat_cb *cb)
{
	if (unlikely(cb->payload_off < cb->l4_off))
		return false;

	if (unlikely(cb->is_icmp_err)) {
		if (unlikely(cb->inner_l3_offset < cb->payload_off))
			return false;
		if (unlikely(cb->inner_l4_offset < cb->inner_l3_offset))
			return false;
		if (unlikely(cb->inner_fragh_off &&
			     cb->inner_fragh_off < cb->inner_l3_offset))
			return false;
		if (unlikely(cb->inner_fragh_off &&
			     cb->inner_fragh_off >= cb->inner_l4_offset))
			return false;
	}

	return true;
}
#endif

static bool ipxlat_v4_validate_addr(__be32 addr4)
{
	return !(ipv4_is_zeronet(addr4) || ipv4_is_loopback(addr4) ||
		 ipv4_is_multicast(addr4) || ipv4_is_lbcast(addr4));
}

/* RFC 7915 Section 4.1 requires ignoring IPv4 options unless an unexpired
 * LSRR/SSRR is present, in which case we must send ICMPv4 SR_FAILED.
 * We intentionally treat malformed option encoding as invalid input and
 * drop early instead of continuing translation.
 */
static int ipxlat_v4_srr_check(struct sk_buff *skb, const struct iphdr *hdr)
{
	const u8 *opt, *end;
	u8 type, len, ptr;

	if (likely(hdr->ihl <= 5))
		return 0;

	opt = (const u8 *)(hdr + 1);
	end = (const u8 *)hdr + (hdr->ihl << 2);

	while (opt < end) {
		type = opt[0];
		if (type == IPOPT_END)
			return 0;
		if (type == IPOPT_NOOP) {
			opt++;
			continue;
		}

		if (unlikely(end - opt < 2))
			return -EINVAL;

		len = opt[1];
		if (unlikely(len < 2 || opt + len > end))
			return -EINVAL;

		if (type == IPOPT_LSRR || type == IPOPT_SSRR) {
			if (unlikely(len < 3))
				return -EINVAL;

			/* points to the beginning of the next IP addr */
			ptr = opt[2];
			if (unlikely(ptr < 4))
				return -EINVAL;
			if (unlikely(ptr > len))
				return 0;
			if (unlikely(ptr > len - 3))
				return -EINVAL;

			ipxlat_mark_icmp_drop(skb, ICMP_DEST_UNREACH,
					      ICMP_SR_FAILED, 0);
			return -EINVAL;
		}

		opt += len;
	}

	return 0;
}

static int ipxlat_v4_pull_l3(struct sk_buff *skb, unsigned int l3_offset,
			     bool inner)
{
	const struct iphdr *iph;
	unsigned int tot_len;
	int l3_len;

	if (unlikely(!pskb_may_pull(skb, l3_offset + sizeof(*iph))))
		return -EINVAL;

	iph = (const struct iphdr *)(skb->data + l3_offset);
	if (unlikely(iph->version != 4 || iph->ihl < 5))
		return -EINVAL;

	l3_len = iph->ihl << 2;
	/* For inner packets use ntohs(iph->tot_len) instead of iph_totlen.
	 * If inner iph->tot_len is zero, iph_totlen would fall back to outer
	 * GSO metadata, which is unrelated to quoted inner packet length.
	 */
	tot_len = unlikely(inner) ? ntohs(iph->tot_len) : iph_totlen(skb, iph);
	if (unlikely(tot_len < l3_len))
		return -EINVAL;

	if (unlikely(!pskb_may_pull(skb, l3_offset + l3_len)))
		return -EINVAL;

	return l3_len;
}

static int ipxlat_v4_pull_l4(struct sk_buff *skb, unsigned int l4_offset,
			     u8 l4_proto, bool *is_icmp_err)
{
	struct icmphdr *icmp;
	struct udphdr *udp;
	struct tcphdr *tcp;

	*is_icmp_err = false;

	switch (l4_proto) {
	case IPPROTO_TCP:
		if (unlikely(!pskb_may_pull(skb, l4_offset + sizeof(*tcp))))
			return -EINVAL;

		tcp = (struct tcphdr *)(skb->data + l4_offset);
		if (unlikely(tcp->doff < 5))
			return -EINVAL;

		return __tcp_hdrlen(tcp);
	case IPPROTO_UDP:
		if (unlikely(!pskb_may_pull(skb, l4_offset + sizeof(*udp))))
			return -EINVAL;

		udp = (struct udphdr *)(skb->data + l4_offset);
		if (unlikely(ntohs(udp->len) < sizeof(*udp)))
			return -EINVAL;

		return sizeof(struct udphdr);
	case IPPROTO_ICMP:
		if (unlikely(!pskb_may_pull(skb, l4_offset + sizeof(*icmp))))
			return -EINVAL;

		icmp = (struct icmphdr *)(skb->data + l4_offset);
		*is_icmp_err = icmp_is_err(icmp->type);
		return sizeof(struct icmphdr);
	default:
		return 0;
	}
}

static int ipxlat_v4_pull_icmp_inner(struct sk_buff *skb,
				     unsigned int inner_l3_off)
{
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct iphdr *inner_l3_hdr;
	unsigned int inner_l4_off;
	int inner_l3_len, err;
	bool is_icmp_err;

	inner_l3_len = ipxlat_v4_pull_l3(skb, inner_l3_off, true);
	if (unlikely(inner_l3_len < 0))
		return inner_l3_len;
	inner_l3_hdr = (const struct iphdr *)(skb->data + inner_l3_off);

	/* accept non-first quoted fragments: only inner L3 is translatable */
	inner_l4_off = inner_l3_off + inner_l3_len;
	cb->inner_l3_offset = inner_l3_off;
	cb->inner_l3_hdr_len = inner_l3_len;
	cb->inner_l4_offset = inner_l4_off;

	if (unlikely(!ipxlat_is_first_frag4(inner_l3_hdr)))
		return 0;

	err = ipxlat_v4_pull_l4(skb, inner_l4_off, inner_l3_hdr->protocol,
				&is_icmp_err);
	if (unlikely(err < 0))
		return err;
	if (unlikely(is_icmp_err))
		return -EINVAL;

	return 0;
}

static int ipxlat_v4_pull_hdrs(struct sk_buff *skb)
{
	const unsigned int l3_off = skb_network_offset(skb);
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	int err, l3_len, l4_len = 0;
	const struct iphdr *l3_hdr;

	/* parse IPv4 header and get its full length including options */
	l3_len = ipxlat_v4_pull_l3(skb, l3_off, false);
	if (unlikely(l3_len < 0))
		return l3_len;
	l3_hdr = ip_hdr(skb);

	if (unlikely(!ipxlat_v4_validate_addr(l3_hdr->daddr)))
		return -EINVAL;

	/* RFC 7915 Section 4.1 */
	if (unlikely(ipxlat_v4_srr_check(skb, l3_hdr)))
		return -EINVAL;
	if (unlikely(l3_hdr->ttl <= 1)) {
		ipxlat_mark_icmp_drop(skb, ICMP_TIME_EXCEEDED, ICMP_EXC_TTL, 0);
		return -EINVAL;
	}

	/* RFC 7915 Section 1.2:
	 * Fragmented ICMP/ICMPv6 packets will not be translated by IP/ICMP
	 * translators.
	 */
	if (unlikely(l3_hdr->protocol == IPPROTO_ICMP &&
		     ip_is_fragment(l3_hdr)))
		return -EINVAL;

	cb->l3_hdr_len = l3_len;
	cb->l4_proto = l3_hdr->protocol;
	cb->l4_off = l3_off + l3_len;
	cb->payload_off = cb->l4_off;
	cb->is_icmp_err = false;

	/* only non fragmented packets or first fragments have transport hdrs */
	if (unlikely(!ipxlat_is_first_frag4(l3_hdr))) {
		if (unlikely(!ipxlat_v4_validate_addr(l3_hdr->saddr)))
			return -EINVAL;
		return 0;
	}

	l4_len = ipxlat_v4_pull_l4(skb, cb->l4_off, l3_hdr->protocol,
				   &cb->is_icmp_err);
	if (unlikely(l4_len < 0))
		return l4_len;

	/* RFC 7915 Section 4.1:
	 * Illegal IPv4 sources are accepted only for ICMPv4 error translation.
	 */
	if (unlikely(!ipxlat_v4_validate_addr(l3_hdr->saddr) &&
		     !cb->is_icmp_err))
		return -EINVAL;

	cb->payload_off = cb->l4_off + l4_len;

	if (unlikely(cb->is_icmp_err)) {
		/* validate the quoted packet in an ICMP error */
		err = ipxlat_v4_pull_icmp_inner(skb, cb->payload_off);
		if (unlikely(err))
			return err;
	}

	return 0;
}

static int ipxlat_v4_validate_icmp_csum(const struct sk_buff *skb)
{
	__sum16 csum;

	/* skip when checksum is not software-owned */
	if (skb->ip_summed != CHECKSUM_NONE)
		return 0;

	/* compute checksum over ICMP header and payload, then fold to 16-bit
	 * Internet checksum to validate it
	 */
	csum = csum_fold(skb_checksum(skb, skb_transport_offset(skb),
				      ipxlat_skb_datagram_len(skb), 0));
	return unlikely(csum) ? -EINVAL : 0;
}

/**
 * ipxlat_v4_validate_skb - validate IPv4 input and fill parser metadata in cb
 * @ipxlat: translator private context
 * @skb: packet to validate
 *
 * Ensures required headers are present/consistent and stores parsed offsets
 * into &struct ipxlat_cb for the translation path.
 *
 * Return: 0 on success, negative errno on validation failure.
 */
int ipxlat_v4_validate_skb(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	struct iphdr *l3_hdr;
	struct udphdr *udph;
	int err;

	if (unlikely(skb_shared(skb)))
		return -EINVAL;

	err = ipxlat_v4_pull_hdrs(skb);
	if (unlikely(err))
		return err;

	skb_set_transport_header(skb, cb->l4_off);

	if (unlikely(cb->is_icmp_err)) {
		if (unlikely(cb->l4_proto != IPPROTO_ICMP)) {
			DEBUG_NET_WARN_ON_ONCE(1);
			return -EINVAL;
		}

		/* Translation path recomputes ICMPv6 checksum from scratch.
		 * Validate here so a corrupted ICMPv4 error is not converted
		 * into a translated packet with a valid checksum.
		 */
		return ipxlat_v4_validate_icmp_csum(skb);
	}

	l3_hdr = ip_hdr(skb);
	if (likely(cb->l4_proto != IPPROTO_UDP))
		return 0;
	if (unlikely(!ipxlat_is_first_frag4(l3_hdr)))
		return 0;

	udph = udp_hdr(skb);
	if (likely(udph->check != 0))
		return 0;

	/* We are in the path where L4 header is present (unfragmented packets
	 * or first fragments) and is UDP.
	 * Fragmented checksum-less IPv4 UDP is rejected because 4->6 cannot
	 * reliably translate it.
	 */
	if (unlikely(ip_is_fragment(l3_hdr))) {
		ipxlat_mark_icmp_drop(skb, ICMP_DEST_UNREACH, ICMP_PKT_FILTERED,
				      0);
		return -EINVAL;
	}

	/* udph->len bounds the span used to compute replacement checksum */
	if (unlikely(ntohs(udph->len) > skb->len - cb->l4_off))
		return -EINVAL;

	cb->udp_zero_csum_len = ntohs(udph->len);

	return 0;
}

static bool ipxlat_v6_validate_saddr(const struct in6_addr *addr6)
{
	return !(ipv6_addr_any(addr6) || ipv6_addr_loopback(addr6) ||
		 ipv6_addr_is_multicast(addr6));
}

static int ipxlat_v6_pull_l4(struct sk_buff *skb, unsigned int l4_offset,
			     u8 l4_proto, bool *is_icmp_err)
{
	struct icmp6hdr *icmp;
	struct udphdr *udp;
	struct tcphdr *tcp;

	*is_icmp_err = false;

	switch (l4_proto) {
	case NEXTHDR_TCP:
		if (unlikely(!pskb_may_pull(skb, l4_offset + sizeof(*tcp))))
			return -EINVAL;
		tcp = (struct tcphdr *)(skb->data + l4_offset);
		return __tcp_hdrlen(tcp);
	case NEXTHDR_UDP:
		if (unlikely(!pskb_may_pull(skb, l4_offset + sizeof(*udp))))
			return -EINVAL;
		udp = (struct udphdr *)(skb->data + l4_offset);
		if (unlikely(ntohs(udp->len) < sizeof(*udp)))
			return -EINVAL;
		return sizeof(struct udphdr);
	case NEXTHDR_ICMP:
		if (unlikely(!pskb_may_pull(skb, l4_offset + sizeof(*icmp))))
			return -EINVAL;
		icmp = (struct icmp6hdr *)(skb->data + l4_offset);
		*is_icmp_err = icmpv6_is_err(icmp->icmp6_type);
		return sizeof(struct icmp6hdr);
	default:
		return 0;
	}
}

/* Basic IPv6 header walk: parse only the packet starting at l3_offset.
 * It does not inspect quoted inner packets carried by ICMP errors.
 */
static int ipxlat_v6_walk_hdrs(struct sk_buff *skb, unsigned int l3_offset,
			       u8 *l4_proto, unsigned int *fhdr_offset,
			       unsigned int *l4_offset, bool *has_l4)
{
	unsigned int frag_hdr_off, l4hdr_off;
	struct frag_hdr *frag;
	struct ipv6hdr *ip6;
	bool first_frag;
	int err;

	/* cannot use default getter because this function is used both for
	 * outer and inner packets
	 */
	ip6 = (struct ipv6hdr *)(skb->data + l3_offset);

	/* if present, locate Fragment Header first because it affects
	 * whether transport headers are available
	 */
	frag_hdr_off = l3_offset;
	err = ipv6_find_hdr(skb, &frag_hdr_off, NEXTHDR_FRAGMENT, NULL, NULL);
	if (unlikely(err < 0 && err != -ENOENT))
		return -EINVAL;

	*has_l4 = true;
	*fhdr_offset = 0;
	if (unlikely(err == NEXTHDR_FRAGMENT)) {
		if (unlikely(!pskb_may_pull(skb, frag_hdr_off + sizeof(*frag))))
			return -EINVAL;
		frag = (struct frag_hdr *)(skb->data + frag_hdr_off);

		/* remember Fragment Header offset for downstream logic */
		*fhdr_offset = frag_hdr_off;
		first_frag = ipxlat_is_first_frag6(frag);

		/* ipv6 forbids chaining FHs */
		if (unlikely(frag->nexthdr == NEXTHDR_FRAGMENT))
			return -EINVAL;

		/* RFC 7915 Section 5.1.1 does not support extension headers
		 * after FH (except NEXTHDR_NONE)
		 */
		if (unlikely(ipv6_ext_hdr(frag->nexthdr) &&
			     frag->nexthdr != NEXTHDR_NONE))
			return -EPROTONOSUPPORT;

		/* non-first fragments do not carry a full transport header */
		if (!first_frag) {
			*l4_proto = frag->nexthdr;
			/* first byte after FH is fragment payload,
			 * not L4 header
			 */
			*l4_offset = frag_hdr_off + sizeof(struct frag_hdr);
			*has_l4 = false;
			return 0;
		}
	}

	/* walk extension headers to terminal protocol and compute offsets used
	 * by validation/translation
	 */
	l4hdr_off = l3_offset;
	err = ipv6_find_hdr(skb, &l4hdr_off, -1, NULL, NULL);
	if (unlikely(err < 0))
		return -EINVAL;

	*l4_proto = err;
	*l4_offset = l4hdr_off;
	return 0;
}

/* RFC 7915 Section 5.1 says a Routing Header with Segments Left != 0
 * must not be translated. We detect it by asking ipv6_find_hdr not to
 * skip RH, then emit ICMPv6 Parameter Problem pointing to segments_left.
 */
static int ipxlat_v6_check_rh(struct sk_buff *skb)
{
	unsigned int rh_off, pointer;
	int flags, nexthdr;

	rh_off = 0;
	flags = IP6_FH_F_SKIP_RH;
	nexthdr = ipv6_find_hdr(skb, &rh_off, NEXTHDR_ROUTING, NULL, &flags);
	if (unlikely(nexthdr < 0 && nexthdr != -ENOENT))
		return -EINVAL;
	if (likely(nexthdr != NEXTHDR_ROUTING))
		return 0;

	pointer = rh_off + offsetof(struct ipv6_rt_hdr, segments_left);
	ipxlat_mark_icmp_drop(skb, ICMPV6_PARAMPROB, ICMPV6_HDR_FIELD, pointer);
	return -EINVAL;
}

static int ipxlat_v6_pull_outer_l3(struct sk_buff *skb)
{
	const unsigned int l3_off = skb_network_offset(skb);
	struct ipv6hdr *l3_hdr;

	if (unlikely(!pskb_may_pull(skb, l3_off + sizeof(*l3_hdr))))
		return -EINVAL;
	l3_hdr = ipv6_hdr(skb);

	/* translator does not support jumbograms; payload_len must match skb */
	if (unlikely(l3_hdr->version != 6 ||
		     skb->len != sizeof(*l3_hdr) +
					 be16_to_cpu(l3_hdr->payload_len) ||
		     !ipxlat_v6_validate_saddr(&l3_hdr->saddr)))
		return -EINVAL;

	if (unlikely(l3_hdr->hop_limit <= 1)) {
		ipxlat_mark_icmp_drop(skb, ICMPV6_TIME_EXCEED,
				      ICMPV6_EXC_HOPLIMIT, 0);
		return -EINVAL;
	}

	return 0;
}

static int ipxlat_v6_pull_icmp_inner(struct sk_buff *skb,
				     unsigned int outer_payload_off)
{
	unsigned int inner_fhdr_off, inner_l4_off;
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	struct ipv6hdr *inner_ip6;
	bool has_l4, is_icmp_err;
	u8 inner_l4_proto;
	int err;

	if (unlikely(!pskb_may_pull(skb,
				    outer_payload_off + sizeof(*inner_ip6))))
		return -EINVAL;

	inner_ip6 = (struct ipv6hdr *)(skb->data + outer_payload_off);
	if (unlikely(inner_ip6->version != 6))
		return -EINVAL;

	err = ipxlat_v6_walk_hdrs(skb, outer_payload_off, &inner_l4_proto,
				  &inner_fhdr_off, &inner_l4_off, &has_l4);
	if (unlikely(err))
		return err;

	cb->inner_l3_offset = outer_payload_off;
	cb->inner_l4_offset = inner_l4_off;
	cb->inner_fragh_off = inner_fhdr_off;
	cb->inner_l4_proto = inner_l4_proto;

	if (likely(has_l4)) {
		err = ipxlat_v6_pull_l4(skb, inner_l4_off, inner_l4_proto,
					&is_icmp_err);
		if (unlikely(err < 0))
			return err;
		if (unlikely(is_icmp_err))
			return -EINVAL;
	}

	return 0;
}

static int ipxlat_v6_pull_hdrs(struct sk_buff *skb)
{
	const unsigned int l3_off = skb_network_offset(skb);
	unsigned int fragh_off, l4_off, payload_off;
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	int l3_len, l4_len, err;
	struct frag_hdr *frag;
	bool has_l4;
	u8 l4_proto;

	/* parse IPv6 base header and perform basic structural checks */
	err = ipxlat_v6_pull_outer_l3(skb);
	if (unlikely(err))
		return err;

	/* walk extension/fragment headers and locate the transport header */
	err = ipxlat_v6_walk_hdrs(skb, l3_off, &l4_proto, &fragh_off, &l4_off,
				  &has_l4);
	/* -EPROTONOSUPPORT means packet layout is syntactically valid but
	 * unsupported by our RFC 7915 path
	 */
	if (unlikely(err == -EPROTONOSUPPORT)) {
		ipxlat_mark_icmp_drop(skb, ICMPV6_DEST_UNREACH,
				      ICMPV6_ADM_PROHIBITED, 0);
		return -EINVAL;
	}
	if (unlikely(err))
		return err;

	l3_len = l4_off - l3_off;
	payload_off = l4_off;

	if (likely(has_l4)) {
		l4_len = ipxlat_v6_pull_l4(skb, l4_off, l4_proto,
					   &cb->is_icmp_err);
		if (unlikely(l4_len < 0))
			return l4_len;
		payload_off += l4_len;
	}

	/* RFC 7915 Section 5.1 */
	err = ipxlat_v6_check_rh(skb);
	if (unlikely(err))
		return err;

	if (unlikely(l4_proto == NEXTHDR_ICMP)) {
		/* A stateless translator cannot reliably translate ICMP
		 * checksum across real IPv6 fragments, so fragmented ICMP is
		 * dropped. A Fragment Header alone, however, is not enough to
		 * decide: so-called atomic fragments (offset=0, M=0) carry a
		 * Fragment Header but are not actually fragmented.
		 */
		if (unlikely(fragh_off)) {
			if (unlikely(!pskb_may_pull(skb,
						    fragh_off + sizeof(*frag))))
				return -EINVAL;

			frag = (struct frag_hdr *)(skb->data + fragh_off);
			if (unlikely(ipxlat_get_frag6_offset(frag) ||
				     (be16_to_cpu(frag->frag_off) & IP6_MF)))
				return -EINVAL;
		}

		if (unlikely(cb->is_icmp_err)) {
			/* validate the quoted packet in an ICMP error */
			err = ipxlat_v6_pull_icmp_inner(skb, payload_off);
			if (unlikely(err))
				return err;
		}
	}

	cb->l4_proto = l4_proto;
	cb->l4_off = l4_off;
	cb->fragh_off = fragh_off;
	cb->payload_off = payload_off;
	cb->l3_hdr_len = l3_len;

	return 0;
}

static int ipxlat_v6_validate_icmp_csum(const struct sk_buff *skb)
{
	struct ipv6hdr *iph6;
	unsigned int len;
	__sum16 csum;

	if (skb->ip_summed != CHECKSUM_NONE)
		return 0;

	iph6 = ipv6_hdr(skb);
	len = ipxlat_skb_datagram_len(skb);
	csum = csum_ipv6_magic(&iph6->saddr, &iph6->daddr, len, NEXTHDR_ICMP,
			       skb_checksum(skb, skb_transport_offset(skb), len,
					    0));

	return unlikely(csum) ? -EINVAL : 0;
}

/**
 * ipxlat_v6_validate_skb - validate IPv6 input and fill parser metadata in cb
 * @skb: packet to validate
 *
 * Ensures required headers are present/consistent and stores parsed offsets
 * into &struct ipxlat_cb for the translation path.
 *
 * Return: 0 on success, negative errno on validation failure.
 */
int ipxlat_v6_validate_skb(struct sk_buff *skb)
{
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	int err;

	if (unlikely(skb_shared(skb)))
		return -EINVAL;

	err = ipxlat_v6_pull_hdrs(skb);
	if (unlikely(err))
		return err;

	skb_set_transport_header(skb, cb->l4_off);

	if (unlikely(cb->is_icmp_err)) {
		if (unlikely(cb->l4_proto != NEXTHDR_ICMP)) {
			DEBUG_NET_WARN_ON_ONCE(1);
			return -EINVAL;
		}

		/* The translated ICMPv4 checksum is recomputed from scratch,
		 * so reject bad ICMPv6 error checksums before conversion.
		 */
		err = ipxlat_v6_validate_icmp_csum(skb);
		if (unlikely(err))
			return err;
	}

	return 0;
}
