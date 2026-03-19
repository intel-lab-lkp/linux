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
	if (unlikely(l3_hdr->ttl <= 1))
		return -EINVAL;

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
	if (unlikely(ip_is_fragment(l3_hdr)))
		return -EINVAL;

	/* udph->len bounds the span used to compute replacement checksum */
	if (unlikely(ntohs(udph->len) > skb->len - cb->l4_off))
		return -EINVAL;

	cb->udp_zero_csum_len = ntohs(udph->len);

	return 0;
}

int ipxlat_v6_validate_skb(struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}
