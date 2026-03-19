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

#include <net/ip.h>
#include <net/ip6_checksum.h>
#include <net/tcp.h>
#include <net/udp.h>

#include "packet.h"
#include "transport.h"

/* set CHECKSUM_PARTIAL metadata for transport checksum completion */
int ipxlat_set_partial_csum(struct sk_buff *skb, u16 csum_offset)
{
	if (likely(skb_partial_csum_set(skb, skb_transport_offset(skb),
					csum_offset)))
		return 0;
	return -EINVAL;
}

static __wsum ipxlat_pseudohdr6_csum(const struct ipv6hdr *hdr)
{
	return ~csum_unfold(csum_ipv6_magic(&hdr->saddr, &hdr->daddr, 0, 0, 0));
}

static __wsum ipxlat_pseudohdr4_csum(const struct iphdr *hdr)
{
	return csum_tcpudp_nofold(hdr->saddr, hdr->daddr, 0, 0, 0);
}

static __sum16 ipxlat_46_update_csum(__sum16 csum16,
				     const struct iphdr *in_ip4,
				     const void *in_l4_hdr,
				     const struct ipv6hdr *out_ip6,
				     const void *out_l4_hdr, size_t l4_hdr_len)
{
	__wsum csum;

	csum = ~csum_unfold(csum16);

	/* replace pseudohdr and L4 header contributions, payload unchanged */
	csum = csum_sub(csum, ipxlat_pseudohdr4_csum(in_ip4));
	csum = csum_sub(csum, csum_partial(in_l4_hdr, l4_hdr_len, 0));
	csum = csum_add(csum, ipxlat_pseudohdr6_csum(out_ip6));
	csum = csum_add(csum, csum_partial(out_l4_hdr, l4_hdr_len, 0));
	return csum_fold(csum);
}

static __sum16 ipxlat_64_update_csum(__sum16 csum16,
				     const struct ipv6hdr *in_ip6,
				     const void *in_l4_hdr,
				     size_t in_l4_hdr_len,
				     const struct iphdr *out_ip4,
				     const void *out_l4_hdr,
				     size_t out_l4_hdr_len)
{
	__wsum csum;

	csum = ~csum_unfold(csum16);

	/* only address terms matter because L4 length/proto are unchanged */
	csum = csum_sub(csum, ipxlat_pseudohdr6_csum(in_ip6));
	csum = csum_sub(csum, csum_partial(in_l4_hdr, in_l4_hdr_len, 0));

	csum = csum_add(csum, ipxlat_pseudohdr4_csum(out_ip4));
	csum = csum_add(csum, csum_partial(out_l4_hdr, out_l4_hdr_len, 0));

	return csum_fold(csum);
}

__sum16 ipxlat_l4_csum_ipv6(const struct in6_addr *saddr,
			    const struct in6_addr *daddr,
			    const struct sk_buff *skb, unsigned int l4_off,
			    unsigned int l4_len, u8 proto)
{
	return csum_ipv6_magic(saddr, daddr, l4_len, proto,
			       skb_checksum(skb, l4_off, l4_len, 0));
}

/* Normalize checksum/offload metadata after address-family translation.
 *
 * Translation changes protocol family but keeps transport payload semantics
 * intact, so TCP GSO only needs type remap (gso_from -> gso_to), while ICMP
 * must clear stale GSO state because there is no ICMP GSO transform here.
 *
 * This mirrors forwarding expectations: reject LRO on xmit and clear hash
 * when tuple semantics may have changed (fragments and non-TCP/UDP).
 */
int ipxlat_finalize_offload(struct sk_buff *skb, u8 l4_proto, bool is_fragment,
			    u32 gso_from, u32 gso_to)
{
	struct skb_shared_info *shinfo;

	if (unlikely(skb->ip_summed == CHECKSUM_COMPLETE))
		skb->ip_summed = CHECKSUM_NONE;

	if (!skb_is_gso(skb))
		goto out_hash;

	/* align with forwarding paths that reject LRO skbs before xmit */
	if (unlikely(skb_warn_if_lro(skb)))
		return -EINVAL;

	shinfo = skb_shinfo(skb);
	switch (l4_proto) {
	case IPPROTO_TCP:
		/* segment payload size is unchanged by address-family
		 * translation so there's no need to touch gso_size
		 */
		if (shinfo->gso_type & gso_from) {
			shinfo->gso_type &= ~gso_from;
			shinfo->gso_type |= gso_to;
		} else if (unlikely(!(shinfo->gso_type & gso_to))) {
			return -EOPNOTSUPP;
		}
		break;
	case IPPROTO_UDP:
		break;
	case IPPROTO_ICMP:
		/* for ICMP there is no GSO transform; clear stale offload
		 * metadata so the stack treats it as a normal frame
		 */
		skb_gso_reset(skb);
		break;
	default:
		return -EPROTONOSUPPORT;
	}

out_hash:
	if (unlikely(is_fragment ||
		     (l4_proto != IPPROTO_TCP && l4_proto != IPPROTO_UDP)))
		skb_clear_hash(skb);
	else
		skb_clear_hash_if_not_l4(skb);
	return 0;
}

int ipxlat_46_outer_tcp(struct sk_buff *skb, const struct iphdr *in4)
{
	const struct ipv6hdr *iph6 = ipv6_hdr(skb);
	struct tcphdr *tcp_new = tcp_hdr(skb);
	struct tcphdr tcp_old;
	__sum16 csum16;

	/* CHECKSUM_PARTIAL keeps a pseudohdr seed in check, not a final
	 * transport checksum. For 4->6, we only re-seed it with IPv6 pseudohdr
	 * data and keep completion deferred to offload.
	 */
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		tcp_new->check = ~tcp_v6_check(ipxlat_skb_datagram_len(skb),
					       &iph6->saddr, &iph6->daddr, 0);
		return ipxlat_set_partial_csum(skb,
					       offsetof(struct tcphdr, check));
	}

	/* zeroing check in old/new headers avoids double-accounting it */
	csum16 = tcp_new->check;
	tcp_old = *tcp_new;
	tcp_old.check = 0;
	tcp_new->check = 0;
	tcp_new->check = ipxlat_46_update_csum(csum16, in4,
					       &tcp_old, iph6, tcp_new,
					       sizeof(*tcp_new));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_46_outer_udp(struct sk_buff *skb, const struct iphdr *in4)
{
	const struct ipxlat_cb *cb = ipxlat_skb_cb(skb);
	const struct ipv6hdr *iph6 = ipv6_hdr(skb);
	struct udphdr *udp_new = udp_hdr(skb);
	struct udphdr udp_old;
	__sum16 csum16;

	/* outer path enforces UDP zero-checksum policy in validation */
	if (skb->ip_summed == CHECKSUM_PARTIAL && likely(udp_new->check != 0)) {
		udp_new->check = ~udp_v6_check(ipxlat_skb_datagram_len(skb),
					       &iph6->saddr, &iph6->daddr, 0);
		return ipxlat_set_partial_csum(skb,
					       offsetof(struct udphdr, check));
	}

	/* incoming UDP IPv4 has no checksum (legal in IPv4, not in IPv6) */
	if (unlikely(udp_new->check == 0)) {
		if (unlikely(!cb->udp_zero_csum_len))
			return -EINVAL;

		udp_new->check =
			ipxlat_l4_csum_ipv6(&iph6->saddr, &iph6->daddr, skb,
					    skb_transport_offset(skb),
					    cb->udp_zero_csum_len, IPPROTO_UDP);
		/* 0x0000 on wire means "no checksum"; preserve computed zero */
		if (udp_new->check == 0)
			udp_new->check = CSUM_MANGLED_0;
		skb->ip_summed = CHECKSUM_NONE;
		return 0;
	}

	csum16 = udp_new->check;
	udp_old = *udp_new;
	udp_old.check = 0;
	udp_new->check = 0;
	udp_new->check = ipxlat_46_update_csum(csum16, in4,
					       &udp_old, iph6, udp_new,
					       sizeof(*udp_new));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_46_inner_tcp(struct sk_buff *skb, const struct iphdr *in4,
			const struct ipv6hdr *iph6, struct tcphdr *tcp_new)
{
	struct tcphdr tcp_old;
	__sum16 csum16;

	csum16 = tcp_new->check;
	tcp_old = *tcp_new;
	tcp_old.check = 0;
	tcp_new->check = 0;
	tcp_new->check = ipxlat_46_update_csum(csum16, in4, &tcp_old, iph6,
					       tcp_new, sizeof(*tcp_new));
	return 0;
}

int ipxlat_46_inner_udp(struct sk_buff *skb, const struct iphdr *in4,
			const struct ipv6hdr *iph6, struct udphdr *udp_new)
{
	struct udphdr udp_old;
	__sum16 csum16;

	if (unlikely(udp_new->check == 0))
		return 0;

	csum16 = udp_new->check;
	udp_old = *udp_new;
	udp_old.check = 0;
	udp_new->check = 0;
	udp_new->check = ipxlat_46_update_csum(csum16, in4, &udp_old, iph6,
					       udp_new, sizeof(*udp_new));
	return 0;
}

int ipxlat_64_outer_tcp(struct sk_buff *skb, const struct ipv6hdr *in6)
{
	struct tcphdr tcp_old, *tcp_new;
	__sum16 csum16;

	tcp_new = tcp_hdr(skb);

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		tcp_new->check = ~tcp_v4_check(ipxlat_skb_datagram_len(skb),
					       ip_hdr(skb)->saddr,
					       ip_hdr(skb)->daddr, 0);
		return ipxlat_set_partial_csum(skb,
					       offsetof(struct tcphdr, check));
	}

	csum16 = tcp_new->check;
	tcp_old = *tcp_new;
	tcp_old.check = 0;
	tcp_new->check = 0;
	tcp_new->check = ipxlat_64_update_csum(csum16, in6, &tcp_old,
					       sizeof(tcp_old), ip_hdr(skb),
					       tcp_new, sizeof(*tcp_new));
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_64_outer_udp(struct sk_buff *skb, const struct ipv6hdr *in6)
{
	struct udphdr udp_old, *udp_new;
	__sum16 csum16;

	udp_new = udp_hdr(skb);

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		udp_new->check = ~udp_v4_check(ipxlat_skb_datagram_len(skb),
					       ip_hdr(skb)->saddr,
					       ip_hdr(skb)->daddr, 0);
		return ipxlat_set_partial_csum(skb,
					       offsetof(struct udphdr, check));
	}

	csum16 = udp_new->check;
	udp_old = *udp_new;
	udp_old.check = 0;
	udp_new->check = 0;
	udp_new->check = ipxlat_64_update_csum(csum16, in6, &udp_old,
					       sizeof(udp_old), ip_hdr(skb),
					       udp_new, sizeof(*udp_new));
	if (udp_new->check == 0)
		udp_new->check = CSUM_MANGLED_0;
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_64_inner_tcp(struct sk_buff *skb, const struct ipv6hdr *in6,
			const struct iphdr *out4, struct tcphdr *tcp_new)
{
	struct tcphdr tcp_old;
	__sum16 csum16;

	csum16 = tcp_new->check;
	tcp_old = *tcp_new;
	tcp_old.check = 0;
	tcp_new->check = 0;
	tcp_new->check = ipxlat_64_update_csum(csum16, in6, &tcp_old,
					       sizeof(tcp_old), out4, tcp_new,
					       sizeof(*tcp_new));
	return 0;
}

int ipxlat_64_inner_udp(struct sk_buff *skb, const struct ipv6hdr *in6,
			const struct iphdr *out4, struct udphdr *udp_new)
{
	struct udphdr udp_old;
	__sum16 csum16;

	csum16 = udp_new->check;
	udp_old = *udp_new;
	udp_old.check = 0;
	udp_new->check = 0;
	udp_new->check = ipxlat_64_update_csum(csum16, in6, &udp_old,
					       sizeof(udp_old), out4, udp_new,
					       sizeof(*udp_new));
	if (udp_new->check == 0)
		udp_new->check = CSUM_MANGLED_0;
	return 0;
}

int ipxlat_46_icmp(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	return -EPROTONOSUPPORT;
}

int ipxlat_64_icmp(struct ipxlat_priv *ipxlat, struct sk_buff *skb,
		   const struct ipv6hdr *outer6)
{
	return -EPROTONOSUPPORT;
}
