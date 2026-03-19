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
#include <linux/icmpv6.h>

#include "icmp.h"
#include "packet.h"
#include "transport.h"

static int ipxlat_46_map_icmp_info_type_code(const struct icmphdr *in,
					     struct icmp6hdr *out)
{
	switch (in->type) {
	case ICMP_ECHO:
		out->icmp6_type = ICMPV6_ECHO_REQUEST;
		out->icmp6_code = 0;
		out->icmp6_identifier = in->un.echo.id;
		out->icmp6_sequence = in->un.echo.sequence;
		return 0;
	case ICMP_ECHOREPLY:
		out->icmp6_type = ICMPV6_ECHO_REPLY;
		out->icmp6_code = 0;
		out->icmp6_identifier = in->un.echo.id;
		out->icmp6_sequence = in->un.echo.sequence;
		return 0;
	}

	return -EPROTONOSUPPORT;
}

static void ipxlat_46_icmp_info_update_csum(const struct icmphdr *icmp4,
					    struct icmp6hdr *icmp6,
					    const struct ipv6hdr *ip6,
					    const struct sk_buff *skb,
					    unsigned int l4_off)
{
	struct icmp6hdr icmp6_zero;
	struct icmphdr icmp4_zero;
	__wsum csum;

	icmp4_zero = *icmp4;
	icmp4_zero.checksum = 0;
	icmp6_zero = *icmp6;
	icmp6_zero.icmp6_cksum = 0;
	csum = ~csum_unfold(icmp4->checksum);
	csum = csum_sub(csum, csum_partial(&icmp4_zero, sizeof(icmp4_zero), 0));
	csum = csum_add(csum, csum_partial(&icmp6_zero, sizeof(icmp6_zero), 0));
	icmp6->icmp6_cksum = csum_ipv6_magic(&ip6->saddr, &ip6->daddr,
					     skb->len - l4_off,
					     IPPROTO_ICMPV6, csum);
}

static int ipxlat_46_icmp_info_outer(struct sk_buff *skb)
{
	const unsigned int l4_off = skb_transport_offset(skb);
	const struct icmphdr icmp4 = *icmp_hdr(skb);
	const struct ipv6hdr *ip6 = ipv6_hdr(skb);
	struct icmp6hdr *icmp6 = icmp6_hdr(skb);
	int err;

	err = ipxlat_46_map_icmp_info_type_code(&icmp4, icmp6);
	if (unlikely(err))
		return -EINVAL;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		icmp6->icmp6_cksum = ~csum_ipv6_magic(&ip6->saddr, &ip6->daddr,
						      skb->len - l4_off,
						      IPPROTO_ICMPV6, 0);
		return ipxlat_set_partial_csum(skb, offsetof(struct icmp6hdr,
							   icmp6_cksum));
	}

	ipxlat_46_icmp_info_update_csum(&icmp4, icmp6, ip6, skb, l4_off);
	skb->ip_summed = CHECKSUM_NONE;
	return 0;
}

int ipxlat_46_icmp(struct ipxlat_priv *ipxl, struct sk_buff *skb)
{
	if (unlikely(ipxlat_skb_cb(skb)->is_icmp_err))
		return -EPROTONOSUPPORT;

	return ipxlat_46_icmp_info_outer(skb);
}
