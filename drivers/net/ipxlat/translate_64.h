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

#ifndef _NET_IPXLAT_TRANSLATE_64_H_
#define _NET_IPXLAT_TRANSLATE_64_H_

#include "ipxlpriv.h"

struct sk_buff;
struct iphdr;
struct ipv6hdr;

/**
 * ipxlat_64_build_l3 - build translated outer IPv4 header from IPv6 metadata
 * @iph4: output IPv4 header
 * @iph6: source IPv6 header
 * @tot_len: resulting IPv4 total length
 * @frag_off: resulting IPv4 fragment offset/flags
 * @protocol: resulting IPv4 L4 protocol
 * @saddr: resulting IPv4 source address
 * @daddr: resulting IPv4 destination address
 * @ttl: resulting IPv4 TTL
 * @id: resulting IPv4 identification field
 */
void ipxlat_64_build_l3(struct iphdr *iph4, const struct ipv6hdr *iph6,
			unsigned int tot_len, __be16 frag_off, u8 protocol,
			__be32 saddr, __be32 daddr, u8 ttl, __be16 id);

/**
 * ipxlat_64_translate - translate outer packet from IPv6 to IPv4 in place
 * @ipxlat: translator private context
 * @skb: packet to translate
 *
 * Return: 0 on success, negative errno on translation failure.
 */
int ipxlat_64_translate(struct ipxlat_priv *ipxlat, struct sk_buff *skb);

/**
 * ipxlat_64_map_nexthdr_proto - map IPv6 nexthdr to IPv4 L4 protocol
 * @nexthdr: IPv6 next-header value
 *
 * Return: IPv4 protocol value corresponding to @nexthdr.
 */
u8 ipxlat_64_map_nexthdr_proto(u8 nexthdr);

#endif /* _NET_IPXLAT_TRANSLATE_64_H_ */
