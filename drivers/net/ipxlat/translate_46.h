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

#ifndef _NET_IPXLAT_TRANSLATE_46_H_
#define _NET_IPXLAT_TRANSLATE_46_H_

#include "ipxlpriv.h"

struct iphdr;
struct ipv6hdr;
struct frag_hdr;
struct sk_buff;

/**
 * ipxlat_46_map_proto_to_nexthdr - map IPv4 L4 protocol to IPv6 nexthdr
 * @protocol: IPv4 L4 protocol
 *
 * Return: IPv6 next-header value corresponding to @protocol.
 */
u8 ipxlat_46_map_proto_to_nexthdr(u8 protocol);

/**
 * ipxlat_46_build_frag_hdr - build IPv6 Fragment Header from IPv4 fragment info
 * @fh6: output IPv6 fragment header
 * @hdr4: source IPv4 header
 * @l4_proto: original IPv4 L4 protocol
 */
void ipxlat_46_build_frag_hdr(struct frag_hdr *fh6, const struct iphdr *hdr4,
			      u8 l4_proto);

/**
 * ipxlat_46_build_l3 - build translated outer IPv6 header from IPv4 metadata
 * @iph6: output IPv6 header
 * @iph4: source IPv4 header
 * @payload_len: IPv6 payload length
 * @nexthdr: resulting IPv6 nexthdr
 * @hop_limit: resulting IPv6 hop limit
 */
void ipxlat_46_build_l3(struct ipv6hdr *iph6, const struct iphdr *iph4,
			unsigned int payload_len, u8 nexthdr, u8 hop_limit);

/**
 * ipxlat_46_lookup_pmtu6 - lookup post-translation IPv6 PMTU for a 4->6 packet
 * @ipxlat: translator private context
 * @skb: packet being translated
 * @in4: source IPv4 header snapshot
 *
 * Return: effective PMTU clamped against translator device MTU.
 */
unsigned int ipxlat_46_lookup_pmtu6(struct ipxlat_priv *ipxlat,
				    const struct sk_buff *skb,
				    const struct iphdr *in4);

/**
 * ipxlat_46_translate - translate outer packet from IPv4 to IPv6 in place
 * @ipxlat: translator private context
 * @skb: packet to translate
 *
 * Return: 0 on success, negative errno on translation failure.
 */
int ipxlat_46_translate(struct ipxlat_priv *ipxlat, struct sk_buff *skb);

#endif /* _NET_IPXLAT_TRANSLATE_46_H_ */
