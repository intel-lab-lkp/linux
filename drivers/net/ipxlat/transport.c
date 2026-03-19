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
