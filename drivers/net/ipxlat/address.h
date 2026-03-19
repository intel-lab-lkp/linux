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

#ifndef _NET_IPXLAT_ADDRESS_H_
#define _NET_IPXLAT_ADDRESS_H_

#include <linux/ip.h>
#include <net/ipv6.h>

#include "ipxlpriv.h"

/**
 * ipxlat_46_convert_addr - translate one IPv4 address into RFC 6052 IPv6 form
 * @xlat_prefix6: configured RFC 6052 prefix
 * @addr4: IPv4 address to convert
 * @addr6: output IPv6 address
 */
void ipxlat_46_convert_addr(const struct ipv6_prefix *xlat_prefix6,
			    __be32 addr4, struct in6_addr *addr6);

/**
 * ipxlat_64_convert_addrs - translate outer IPv6 endpoints into IPv4 pair
 * @xlat_prefix6: configured RFC 6052 prefix
 * @hdr6: source IPv6 header
 * @icmp_err: source packet is ICMPv6 error
 * @src: output IPv4 source address
 * @dst: output IPv4 destination address
 *
 * Return: 0 on success, negative errno on non-translatable addresses.
 */
int ipxlat_64_convert_addrs(const struct ipv6_prefix *xlat_prefix6,
			    const struct ipv6hdr *hdr6, bool icmp_err,
			    __be32 *src, __be32 *dst);

/**
 * ipxlat_46_convert_addrs - translate outer IPv4 endpoints into IPv6 pair
 * @xlat_prefix6: configured RFC 6052 prefix
 * @iph4: source IPv4 header
 * @iph6: output IPv6 header (only saddr/daddr are updated)
 */
static inline void
ipxlat_46_convert_addrs(const struct ipv6_prefix *xlat_prefix6,
			const struct iphdr *iph4, struct ipv6hdr *iph6)
{
	ipxlat_46_convert_addr(xlat_prefix6, iph4->saddr, &iph6->saddr);
	ipxlat_46_convert_addr(xlat_prefix6, iph4->daddr, &iph6->daddr);
}

#endif /* _NET_IPXLAT_ADDRESS_H_ */
