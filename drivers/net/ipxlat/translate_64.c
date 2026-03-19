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

#include <linux/icmpv6.h>
#include <net/ip.h>

#include "translate_64.h"
#include "address.h"
#include "packet.h"
#include "transport.h"

u8 ipxlat_64_map_nexthdr_proto(u8 nexthdr)
{
	return (nexthdr == NEXTHDR_ICMP) ? IPPROTO_ICMP : nexthdr;
}

void ipxlat_64_build_l3(struct iphdr *iph4, const struct ipv6hdr *iph6,
			unsigned int tot_len, __be16 frag_off, u8 protocol,
			__be32 saddr, __be32 daddr, u8 ttl, __be16 id)
{
	iph4->version = 4;
	iph4->ihl = 5;
	iph4->tos = ipxlat_get_ipv6_tclass(iph6);
	iph4->tot_len = cpu_to_be16(tot_len);
	iph4->frag_off = frag_off;
	iph4->ttl = ttl;
	iph4->protocol = protocol;
	iph4->saddr = saddr;
	iph4->daddr = daddr;
	iph4->id = id;
	iph4->check = 0;
	iph4->check = ip_fast_csum(iph4, iph4->ihl);
}

static __be16 ipxlat_64_build_frag_off(const struct sk_buff *skb,
				       const struct frag_hdr *frag6,
				       u8 l4_proto)
{
	bool df, mf, over_mtu;
	u16 frag_offset;

	/* preserve real IPv6 fragmentation state with a Fragment Header */
	if (frag6) {
		mf = !!(be16_to_cpu(frag6->frag_off) & IP6_MF);
		frag_offset = ipxlat_get_frag6_offset(frag6);
		return ipxlat_build_frag4_offset(false, mf, frag_offset);
	}

	/* frag_list implies segmented payload emitted as fragments */
	if (skb_has_frag_list(skb))
		return ipxlat_build_frag4_offset(false, false, 0);

	if (skb_is_gso(skb)) {
		/* GSO frames are one datagram here; set DF only for TCP
		 * when later segmentation exceeds IPv6 minimum MTU
		 */
		df = (l4_proto == IPPROTO_TCP) &&
		     (ipxlat_skb_cb(skb)->payload_off +
			      skb_shinfo(skb)->gso_size >
		      (IPV6_MIN_MTU - sizeof(struct iphdr)));
		return ipxlat_build_frag4_offset(df, false, 0);
	}

	over_mtu = skb->len > (IPV6_MIN_MTU - sizeof(struct iphdr));
	return ipxlat_build_frag4_offset(over_mtu, false, 0);
}

/**
 * ipxlat_64_translate - translate one validated packet from IPv6 to IPv4
 * @ipxlat: translator private context
 * @skb: packet to translate
 *
 * Rewrites outer L3 in place, rebases cached offsets and translates L4 on
 * first fragments only.
 *
 * Return: 0 on success, negative errno on translation failure.
 */
int ipxlat_64_translate(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	unsigned int min_l4_len, old_l3_len, new_l3_len;
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	struct ipv6hdr outer6 = *ipv6_hdr(skb);
	bool is_icmp_err, has_frag, first_frag;
	u8 in_l4_proto, out_l4_proto;
	struct frag_hdr frag_copy;
	struct frag_hdr *frag6;
	__be32 saddr, daddr;
	__be16 frag_off, id;
	struct iphdr *iph4;
	int l3_delta, err;

	/* snapshot original outer IPv6 fields before L3 rewrite */
	frag6 = cb->fragh_off ? (struct frag_hdr *)(skb->data + cb->fragh_off) :
				NULL;
	has_frag = !!frag6;
	in_l4_proto = cb->l4_proto;
	is_icmp_err = cb->is_icmp_err;
	out_l4_proto = ipxlat_64_map_nexthdr_proto(in_l4_proto);

	old_l3_len = cb->l3_hdr_len;
	new_l3_len = sizeof(struct iphdr);
	l3_delta = (int)new_l3_len - (int)old_l3_len;

	if (unlikely(has_frag))
		frag_copy = *frag6;
	first_frag = ipxlat_is_first_frag6(has_frag ? &frag_copy : NULL);

	if (unlikely(is_icmp_err)) {
		if (unlikely(in_l4_proto != NEXTHDR_ICMP))
			return -EINVAL;
	}

	/* derive translated IPv4 endpoints */
	err = ipxlat_64_convert_addrs(&ipxlat->xlat_prefix6, &outer6,
				      is_icmp_err, &saddr, &daddr);
	if (unlikely(err))
		return err;

	/* replace outer IPv6 hdr with IPv4 hdr in-place */
	skb_pull(skb, old_l3_len);
	skb_push(skb, new_l3_len);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, new_l3_len);
	skb->protocol = htons(ETH_P_IP);

	/* Rebase cached offsets after L3 size delta.
	 * For outer 6->4 translation this should not underflow: cached offsets
	 * were built from l3_off + ip6_len (+ ...), and
	 * delta = sizeof(struct iphdr) - ip6_len, so ip6_len cancels out after
	 * rebasing. A failure here means internal metadata inconsistency, not
	 * a packet validation outcome.
	 */
	err = ipxlat_cb_rebase_offsets(cb, l3_delta);
	if (unlikely(err)) {
		DEBUG_NET_WARN_ON_ONCE(1);
		return err;
	}

	cb->l3_hdr_len = sizeof(struct iphdr);
	cb->fragh_off = 0;
	cb->l4_proto = out_l4_proto;
	DEBUG_NET_WARN_ON_ONCE(!ipxlat_cb_offsets_valid(cb));

	/* build outer IPv4 base hdr from translated IPv6 fields */
	iph4 = ip_hdr(skb);
	frag_off = ipxlat_64_build_frag_off(skb, has_frag ? &frag_copy : NULL,
					    out_l4_proto);
	/* when source had Fragment Header we preserve its identification;
	 * otherwise allocate a fresh IPv4 ID for the translated packet
	 */
	id = has_frag ? cpu_to_be16(be32_to_cpu(frag_copy.identification)) : 0;
	ipxlat_64_build_l3(iph4, &outer6, skb->len, frag_off,
			   out_l4_proto, saddr, daddr,
			   outer6.hop_limit - 1, id);

	if (likely(!has_frag)) {
		iph4->id = 0;
		__ip_select_ident(dev_net(ipxlat->dev), iph4, 1);
		iph4->check = 0;
		iph4->check = ip_fast_csum(iph4, iph4->ihl);
	}

	/* non-first fragments have no transport header to translate */
	if (unlikely(!first_frag))
		goto out;

	/* ensure transport bytes are writable before L4 csum/proto rewrites */
	min_l4_len = ipxlat_l4_min_len(out_l4_proto);
	if (unlikely(skb_ensure_writable(skb, skb_transport_offset(skb) +
						      min_l4_len)))
		return -ENOMEM;

	/* translate transport hdr and pseudohdr dependent checksums */
	switch (out_l4_proto) {
	case IPPROTO_TCP:
		err = ipxlat_64_outer_tcp(skb, &outer6);
		break;
	case IPPROTO_UDP:
		err = ipxlat_64_outer_udp(skb, &outer6);
		break;
	case IPPROTO_ICMP:
		err = ipxlat_64_icmp(ipxlat, skb, &outer6);
		break;
	default:
		err = 0;
		break;
	}
	if (unlikely(err))
		return err;

out:
	/* normalize checksum/offload metadata for the translated frame */
	return ipxlat_finalize_offload(skb, out_l4_proto, ip_is_fragment(iph4),
				       SKB_GSO_TCPV6, SKB_GSO_TCPV4);
}
