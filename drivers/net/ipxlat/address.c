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

#include "address.h"

static bool ipxlat_prefix6_contains(const struct ipv6_prefix *prefix,
				    const struct in6_addr *addr)
{
	return ipv6_prefix_equal(&prefix->addr, addr, prefix->len);
}

static __be32 ipxlat_64_extract_addr(const struct in6_addr *src,
				     unsigned int q1, unsigned int q2,
				     unsigned int q3, unsigned int q4)
{
	q1 = src->s6_addr[q1];
	q2 = src->s6_addr[q2];
	q3 = src->s6_addr[q3];
	q4 = src->s6_addr[q4];
	return htonl((q1 << 24) | (q2 << 16) | (q3 << 8) | q4);
}

static void ipxlat_46_embed_addr(__be32 __src, struct in6_addr *dst,
				 unsigned int q1, unsigned int q2,
				 unsigned int q3, unsigned int q4)
{
	u32 src = ntohl(__src);

	dst->s6_addr[q1] = ((src >> 24) & 0xFF);
	dst->s6_addr[q2] = ((src >> 16) & 0xFF);
	dst->s6_addr[q3] = ((src >> 8) & 0xFF);
	dst->s6_addr[q4] = ((src) & 0xFF);
}

void ipxlat_46_convert_addr(const struct ipv6_prefix *xlat_prefix6,
			    __be32 addr4, struct in6_addr *addr6)
{
	*addr6 = xlat_prefix6->addr;

	switch (xlat_prefix6->len) {
	case 96:
		addr6->s6_addr32[3] = addr4;
		return;
	case 64:
		ipxlat_46_embed_addr(addr4, addr6, 9, 10, 11, 12);
		return;
	case 56:
		ipxlat_46_embed_addr(addr4, addr6, 7, 9, 10, 11);
		return;
	case 48:
		ipxlat_46_embed_addr(addr4, addr6, 6, 7, 9, 10);
		return;
	case 40:
		ipxlat_46_embed_addr(addr4, addr6, 5, 6, 7, 9);
		return;
	case 32:
		addr6->s6_addr32[1] = addr4;
		return;
	}

	DEBUG_NET_WARN_ON_ONCE(1);
}

int ipxlat_64_convert_addrs(const struct ipv6_prefix *xlat_prefix6,
			    const struct ipv6hdr *hdr6, bool icmp_err,
			    __be32 *src, __be32 *dst)
{
	bool src_ok;

	src_ok = ipxlat_prefix6_contains(xlat_prefix6, &hdr6->saddr);
	if (unlikely(!src_ok && !icmp_err))
		return -EINVAL;
	if (unlikely(!ipxlat_prefix6_contains(xlat_prefix6, &hdr6->daddr)))
		return -EINVAL;

	switch (xlat_prefix6->len) {
	case 96:
		if (likely(src_ok))
			*src = hdr6->saddr.s6_addr32[3];
		*dst = hdr6->daddr.s6_addr32[3];
		break;
	case 64:
		if (likely(src_ok))
			*src = ipxlat_64_extract_addr(&hdr6->saddr, 9, 10, 11,
						      12);
		*dst = ipxlat_64_extract_addr(&hdr6->daddr, 9, 10, 11, 12);
		break;
	case 56:
		if (likely(src_ok))
			*src = ipxlat_64_extract_addr(&hdr6->saddr, 7,
						      9, 10, 11);
		*dst = ipxlat_64_extract_addr(&hdr6->daddr, 7, 9, 10, 11);
		break;
	case 48:
		if (likely(src_ok))
			*src = ipxlat_64_extract_addr(&hdr6->saddr, 6,
						      7, 9, 10);
		*dst = ipxlat_64_extract_addr(&hdr6->daddr, 6, 7, 9, 10);
		break;
	case 40:
		if (likely(src_ok))
			*src = ipxlat_64_extract_addr(&hdr6->saddr, 5, 6, 7, 9);
		*dst = ipxlat_64_extract_addr(&hdr6->daddr, 5, 6, 7, 9);
		break;
	case 32:
		if (likely(src_ok))
			*src = hdr6->saddr.s6_addr32[1];
		*dst = hdr6->daddr.s6_addr32[1];
		break;
	default:
		DEBUG_NET_WARN_ON_ONCE(1);
		return -EINVAL;
	}

	/* keep 6->4 ICMP error translation functional even when the ICMPv6
	 * source is not xlat_prefix6-mapped (for example, stack-generated PTB)
	 */
	if (unlikely(!src_ok))
		*src = htonl(INADDR_DUMMY);

	return 0;
}
