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

#ifndef _NET_IPXLAT_ICMP_H_
#define _NET_IPXLAT_ICMP_H_

#include <linux/ipv6.h>

#include "ipxlpriv.h"

/**
 * ipxlat_46_icmp - translate ICMP informational payload
 *		    after outer 4->6 rewrite
 * @ipxl: translator private context
 * @skb: packet carrying ICMPv4 transport payload
 *
 * Return: 0 on success, negative errno on translation failure.
 */
int ipxlat_46_icmp(struct ipxlat_priv *ipxl, struct sk_buff *skb);

/**
 * ipxlat_64_icmp - translate ICMP informational payload
 *		    after outer 6->4 rewrite
 * @ipxlat: translator private context
 * @skb: packet carrying ICMPv6 transport payload
 * @in6: snapshot of original outer IPv6 header
 *
 * Return: 0 on success, negative errno on translation failure.
 */
int ipxlat_64_icmp(struct ipxlat_priv *ipxlat, struct sk_buff *skb,
		   const struct ipv6hdr *in6);

#endif /* _NET_IPXLAT_ICMP_H_ */
