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

#ifndef _NET_IPXLAT_DISPATCH_H_
#define _NET_IPXLAT_DISPATCH_H_

#include "ipxlpriv.h"

struct sk_buff;

/**
 * enum ipxlat_action - result of packet translation dispatch
 * @IPXLAT_ACT_DROP: drop the packet
 * @IPXLAT_ACT_FWD: packet translated and ready for forward reinjection
 * @IPXLAT_ACT_PRE_FRAG: packet must be fragmented before 4->6 translation
 * @IPXLAT_ACT_ICMP_ERR: drop packet and emit translator-generated ICMP error
 */
enum ipxlat_action {
	IPXLAT_ACT_DROP,
	IPXLAT_ACT_FWD,
	IPXLAT_ACT_PRE_FRAG,
	IPXLAT_ACT_ICMP_ERR,
};

/**
 * ipxlat_mark_icmp_drop - cache translator-generated ICMP action in skb cb
 * @skb: packet being rejected
 * @type: ICMP type to emit
 * @code: ICMP code to emit
 * @info: ICMP auxiliary info (pointer/MTU), host-endian
 *
 * This does not emit immediately; dispatch consumes the mark later and sends
 * the ICMP error through the appropriate address family path.
 */
void ipxlat_mark_icmp_drop(struct sk_buff *skb, u8 type, u8 code, u32 info);

/**
 * ipxlat_translate - validate/translate one packet and return next action
 * @ipxlat: translator private context
 * @skb: packet to process
 *
 * Return: one of &enum ipxlat_action.
 */
enum ipxlat_action ipxlat_translate(struct ipxlat_priv *ipxlat,
				    struct sk_buff *skb);

/**
 * ipxlat_process_skb - top-level packet handler for ndo_start_xmit/reinjection
 * @ipxlat: translator private context
 * @skb: packet to process
 * @allow_pre_frag: allow 4->6 pre-fragment action for this invocation
 *
 * The function always consumes @skb directly or through fragmentation
 * callback/reinjection paths.
 *
 * Return: 0 on success, negative errno on processing failure.
 */
int ipxlat_process_skb(struct ipxlat_priv *ipxlat, struct sk_buff *skb,
		       bool allow_pre_frag);

#endif /* _NET_IPXLAT_DISPATCH_H_ */
