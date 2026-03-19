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

#include "dispatch.h"
#include "packet.h"
#include "translate_46.h"
#include "translate_64.h"

static enum ipxlat_action
ipxlat_resolve_failed_action(const struct sk_buff *skb)
{
	return IPXLAT_ACT_DROP;
}

enum ipxlat_action ipxlat_translate(struct ipxlat_priv *ipxlat,
				    struct sk_buff *skb)
{
	const u16 proto = ntohs(skb->protocol);

	memset(skb->cb, 0, sizeof(struct ipxlat_cb));

	if (proto == ETH_P_IPV6) {
		if (unlikely(ipxlat_v6_validate_skb(skb)) ||
		    unlikely(ipxlat_64_translate(ipxlat, skb)))
			return ipxlat_resolve_failed_action(skb);

		return IPXLAT_ACT_FWD;
	} else if (likely(proto == ETH_P_IP)) {
		if (unlikely(ipxlat_v4_validate_skb(ipxlat, skb)))
			return ipxlat_resolve_failed_action(skb);

		if (unlikely(ipxlat_46_translate(ipxlat, skb)))
			return ipxlat_resolve_failed_action(skb);

		return IPXLAT_ACT_FWD;
	}

	return IPXLAT_ACT_DROP;
}

/* mark current skb as drop-with-icmp and cache type/code/info for dispatch */
void ipxlat_mark_icmp_drop(struct sk_buff *skb, u8 type, u8 code, u32 info)
{
	struct ipxlat_cb *cb = ipxlat_skb_cb(skb);

	cb->emit_icmp_err = true;
	cb->icmp_err.type = type;
	cb->icmp_err.code = code;
	cb->icmp_err.info = info;
}

static void ipxlat_forward_pkt(struct ipxlat_priv *ipxlat, struct sk_buff *skb)
{
	const unsigned int len = skb->len;
	int err;

	/* reinject as a fresh packet with scrubbed metadata */
	skb_set_queue_mapping(skb, 0);
	skb_scrub_packet(skb, false);

	err = gro_cells_receive(&ipxlat->gro_cells, skb);
	if (likely(err == NET_RX_SUCCESS))
		dev_dstats_rx_add(ipxlat->dev, len);
	/* on failure gro_cells updates rx drop stats internally */
}

int ipxlat_process_skb(struct ipxlat_priv *ipxlat, struct sk_buff *skb,
		       bool allow_pre_frag)
{
	enum ipxlat_action action;
	int err = -EINVAL;

	(void)allow_pre_frag;

	action = ipxlat_translate(ipxlat, skb);
	switch (action) {
	case IPXLAT_ACT_FWD:
		dev_dstats_tx_add(ipxlat->dev, skb->len);
		ipxlat_forward_pkt(ipxlat, skb);
		return 0;
	case IPXLAT_ACT_DROP:
		goto drop_free;
	default:
		DEBUG_NET_WARN_ON_ONCE(1);
		goto drop_free;
	}

drop_free:
	dev_dstats_tx_dropped(ipxlat->dev);
	kfree_skb(skb);
	return err;
}
