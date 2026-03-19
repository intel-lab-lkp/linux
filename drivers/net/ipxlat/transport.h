/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef _NET_IPXLAT_TRANSPORT_H_
#define _NET_IPXLAT_TRANSPORT_H_

#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>

/**
 * ipxlat_l4_min_len - minimum transport header size for protocol
 * @protocol: transport protocol identifier
 *
 * Return: minimum header length for @protocol, or 0 when unsupported.
 */
static inline unsigned int ipxlat_l4_min_len(u8 protocol)
{
	switch (protocol) {
	case IPPROTO_TCP:
		return sizeof(struct tcphdr);
	case IPPROTO_UDP:
		return sizeof(struct udphdr);
	case IPPROTO_ICMP:
		return sizeof(struct icmphdr);
	default:
		return 0;
	}
}

/**
 * ipxlat_set_partial_csum - program CHECKSUM_PARTIAL metadata on skb
 * @skb: packet with transport checksum field
 * @csum_offset: offset of checksum field within transport header
 *
 * Return: 0 on success, negative errno on invalid skb state.
 */
int ipxlat_set_partial_csum(struct sk_buff *skb, u16 csum_offset);

/**
 * ipxlat_l4_csum_ipv6 - compute full L4 checksum with IPv6 pseudo-header
 * @saddr: IPv6 source address
 * @daddr: IPv6 destination address
 * @skb: packet buffer
 * @l4_off: transport header offset
 * @l4_len: transport span (header + payload)
 * @proto: transport protocol
 *
 * Return: folded checksum value covering pseudo-header and transport payload.
 */
__sum16 ipxlat_l4_csum_ipv6(const struct in6_addr *saddr,
			    const struct in6_addr *daddr,
			    const struct sk_buff *skb, unsigned int l4_off,
			    unsigned int l4_len, u8 proto);

/**
 * ipxlat_finalize_offload - normalize checksum/GSO metadata after translation
 * @skb: translated packet
 * @l4_proto: resulting transport protocol
 * @is_fragment: resulting packet is fragmented
 * @gso_from: input TCP GSO type bit
 * @gso_to: output TCP GSO type bit
 *
 * Converts TCP GSO family bits and clears stale checksum/hash state when
 * offload metadata cannot be preserved across address-family translation.
 *
 * Return: 0 on success, negative errno on unsupported/offload-incompatible
 * input.
 */
int ipxlat_finalize_offload(struct sk_buff *skb, u8 l4_proto, bool is_fragment,
			    u32 gso_from, u32 gso_to);

#endif /* _NET_IPXLAT_TRANSPORT_H_ */
