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

#include <net/ip6_route.h>

#include "address.h"
#include "icmp.h"
#include "packet.h"
#include "transport.h"
#include "translate_46.h"

u8 ipxlat_46_map_proto_to_nexthdr(u8 protocol)
{
	return (protocol == IPPROTO_ICMP) ? NEXTHDR_ICMP : protocol;
}

void ipxlat_46_build_frag_hdr(struct frag_hdr *fh6, const struct iphdr *hdr4,
			      u8 l4_proto)
{
	fh6->nexthdr = ipxlat_46_map_proto_to_nexthdr(l4_proto);
	fh6->reserved = 0;
	fh6->frag_off =
		ipxlat_build_frag6_offset(ipxlat_get_frag4_offset(hdr4),
					  !!(be16_to_cpu(hdr4->frag_off) &
					     IP_MF));
	fh6->identification = cpu_to_be32(be16_to_cpu(hdr4->id));
}

void ipxlat_46_build_l3(struct ipv6hdr *iph6, const struct iphdr *iph4,
			unsigned int payload_len, u8 nexthdr, u8 hop_limit)
{
	iph6->version = 6;
	iph6->priority = iph4->tos >> 4;
	iph6->flow_lbl[0] = (iph4->tos & 0x0F) << 4;
	iph6->flow_lbl[1] = 0;
	iph6->flow_lbl[2] = 0;
	iph6->payload_len = htons(payload_len);
	iph6->nexthdr = nexthdr;
	iph6->hop_limit = hop_limit;
}

/* Lookup post-translation IPv6 PMTU for 4->6 output decisions.
 * Falls back to translator MTU on routing failures and clamps route MTU
 * against translator egress MTU.
 */
unsigned int ipxlat_46_lookup_pmtu6(struct ipxlat_priv *ipxlat,
				    const struct sk_buff *skb,
				    const struct iphdr *in4)
{
	unsigned int mtu6, dev_mtu;
	struct flowi6 fl6 = {};
	struct dst_entry *dst;

	dev_mtu = READ_ONCE(ipxlat->dev->mtu);

	ipxlat_46_convert_addr(&ipxlat->xlat_prefix6, in4->saddr,
			       &fl6.saddr);
	ipxlat_46_convert_addr(&ipxlat->xlat_prefix6, in4->daddr,
			       &fl6.daddr);
	fl6.flowi6_mark = skb->mark;

	dst = ip6_route_output(dev_net(ipxlat->dev), NULL, &fl6);
	if (unlikely(dst->error)) {
		mtu6 = dev_mtu;
		goto out;
	}

	/* Route lookup can return a very large MTU (eg, local/loopback style
	 * routes) that does not reflect the translator egress constraint.
	 * Clamp with the translator device MTU so DF decisions are stable and
	 * pre-fragment planning never targets packets larger than what this
	 * interface can hand to the next stages.
	 */
	mtu6 = min_t(unsigned int, dst_mtu(dst), dev_mtu);

out:
	dst_release(dst);
	return mtu6;
}

/**
 * ipxlat_46_plan_prefrag - plan pre-translation IPv4 fragmentation for 4->6
 * @ipxlat: translator private context
 * @skb: packet being translated
 *
 * Decides whether packet exceeds PMTU/LIM thresholds and, when needed, stores
 * per-skb fragmentation cap in cb->frag_max_size for later ip_do_fragment.
 *
 * Return: 0 on success, negative errno on policy/validation failure.
 */
int ipxlat_46_plan_prefrag(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	unsigned int pkt_len6, pmtu6, threshold6, frag_max_size, pkt_len4,
		old_l3_len, new_l3_len;
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct iphdr *in4 = ip_hdr(skb);
	int l3_delta, frag_l3_delta;

	if (unlikely(cb->frag_max_size)) {
		DEBUG_NET_WARN_ON_ONCE(1);
		cb->frag_max_size = 0;
	}

	pkt_len4 = iph_totlen(skb, in4);
	old_l3_len = cb->l3_hdr_len;
	new_l3_len = sizeof(struct ipv6hdr) +
		     (ip_is_fragment(in4) ? sizeof(struct frag_hdr) : 0);
	l3_delta = (int)new_l3_len - (int)old_l3_len;
	pkt_len6 = pkt_len4 + l3_delta;

	pmtu6 = ipxlat_46_lookup_pmtu6(ipxlat, skb, in4);
	threshold6 = min(pmtu6, READ_ONCE(ipxlat->lowest_ipv6_mtu));

	if (likely(pkt_len6 <= threshold6))
		return 0;

	/* df packets are never locally pre-fragmented */
	if (likely(be16_to_cpu(in4->frag_off) & IP_DF)) {
		/* Let the IPv6 forwarding path raise PTB when needed and rely
		 * on the reverse 6->4 ICMP translation path for feedback.
		 */
		return 0;
	}

	/* df not set: we can fragment */

	frag_l3_delta =
		(int)(sizeof(struct ipv6hdr) + sizeof(struct frag_hdr)) -
		(int)old_l3_len;
	frag_max_size = threshold6 - frag_l3_delta;
	/* store per-skb prefrag cap: ipxlat_46_fragment_pkt will copy it into
	 * IPCB(skb)->frag_max_size before calling ip_do_fragment
	 */
	cb->frag_max_size = min_t(unsigned int, frag_max_size, IP_MAX_MTU);
	return 0;
}

/**
 * ipxlat_46_translate - translate one validated packet from IPv4 to IPv6
 * @ipxlat: translator private context
 * @skb: packet to translate
 *
 * Rewrites outer L3 in place, rebases cached offsets and translates L4 on
 * first fragments only.
 *
 * Return: 0 on success, negative errno on translation failure.
 */
int ipxlat_46_translate(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	unsigned int min_l4_len, old_l3_len, new_l3_len;
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct iphdr outer4 = *ip_hdr(skb);
	const u8 in_l4_proto = cb->l4_proto;
	bool has_frag, first_frag;
	struct frag_hdr *fh6;
	struct ipv6hdr *iph6;
	int l3_delta, err;
	u8 out_l4_proto;

	/* snapshot the original IPv4 header fields before skb layout changes */
	has_frag = ip_is_fragment(&outer4);
	first_frag = ipxlat_is_first_frag4(&outer4);
	out_l4_proto = ipxlat_46_map_proto_to_nexthdr(in_l4_proto);

	old_l3_len = cb->l3_hdr_len;
	new_l3_len = sizeof(struct ipv6hdr) +
		     (has_frag ? sizeof(struct frag_hdr) : 0);
	l3_delta = (int)new_l3_len - (int)old_l3_len;

	/* make room for the new hdrs */
	if (unlikely(skb_cow_head(skb, max_t(int, 0, l3_delta))))
		return -ENOMEM;

	/* replace outer L3 area: drop IPv4 hdr, reserve IPv6(+Frag) hdr */
	skb_pull(skb, old_l3_len);
	skb_push(skb, new_l3_len);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, new_l3_len);
	skb->protocol = htons(ETH_P_IPV6);

	/* build outer IPv6 base hdr from translated IPv4 fields */
	iph6 = ipv6_hdr(skb);
	ipxlat_46_build_l3(iph6, &outer4, skb->len - sizeof(*iph6),
			   out_l4_proto, outer4.ttl - 1);

	/* translate IPv4 endpoints into IPv6 addresses using xlat_prefix6 */
	ipxlat_46_convert_addrs(&ipxlat->xlat_prefix6, &outer4, iph6);

	/* add IPv6 fragment hdr when the IPv4 packet carried fragmentation */
	if (unlikely(has_frag)) {
		iph6->nexthdr = NEXTHDR_FRAGMENT;

		fh6 = (struct frag_hdr *)(iph6 + 1);
		ipxlat_46_build_frag_hdr(fh6, &outer4, in_l4_proto);
		cb->fragh_off = sizeof(struct ipv6hdr);
	}

	/* Rebase cached offsets after L3 size delta.
	 * For outer 4->6 translation this should not underflow: cached offsets
	 * were built from l3_off + ip4_len(+...) and delta = ip6_len - ip4_len,
	 * so ip4_len cancels out after rebasing. A failure here means internal
	 * metadata inconsistency, not a packet validation outcome.
	 */
	err = ipxlat_cb_rebase_offsets(cb, l3_delta);
	if (unlikely(err)) {
		DEBUG_NET_WARN_ON_ONCE(1);
		return err;
	}

	cb->l3_hdr_len = new_l3_len;
	cb->l4_proto = out_l4_proto;
	DEBUG_NET_WARN_ON_ONCE(!ipxlat_cb_offsets_valid(cb));

	/* non-first fragments have no transport header to translate */
	if (unlikely(!first_frag))
		goto out;

	/* ensure transport bytes are writable before L4 csum/proto rewrites */
	min_l4_len = ipxlat_l4_min_len(in_l4_proto);
	if (unlikely(skb_ensure_writable(skb, skb_transport_offset(skb) +
						      min_l4_len)))
		return -ENOMEM;

	/* translate transport hdr and pseudohdr dependent checksums */
	switch (in_l4_proto) {
	case IPPROTO_TCP:
		err = ipxlat_46_outer_tcp(skb, &outer4);
		break;
	case IPPROTO_UDP:
		err = ipxlat_46_outer_udp(skb, &outer4);
		break;
	case IPPROTO_ICMP:
		err = ipxlat_46_icmp(ipxlat, skb);
		break;
	default:
		err = 0;
		break;
	}
	if (unlikely(err))
		return err;

out:
	/* normalize checksum/offload metadata for the translated frame */
	return ipxlat_finalize_offload(skb, in_l4_proto, has_frag,
				       SKB_GSO_TCPV4, SKB_GSO_TCPV6);
}
