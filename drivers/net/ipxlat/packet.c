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

#include "packet.h"

/* Shift cached skb cb offsets by the L3 header delta after in-place rewrite.
 *
 * Translation may replace only the outer L3 header size (4->6 or 6->4), while
 * cached offsets were computed before rewrite. Rebasing applies the same delta
 * to all cached absolute offsets so they still point to the same logical
 * fields in the modified skb.
 *
 * This helper only guards against underflow (< 0). Relative ordering checks
 * are done by ipxlat_cb_offsets_valid.
 */
int ipxlat_cb_rebase_offsets(struct ipxlat_cb *cb, int delta)
{
	int off;

	off = cb->l4_off + delta;
	if (unlikely(off < 0))
		return -EINVAL;
	cb->l4_off = off;

	off = cb->payload_off + delta;
	if (unlikely(off < 0))
		return -EINVAL;
	cb->payload_off = off;

	if (unlikely(cb->is_icmp_err)) {
		off = cb->inner_l3_offset + delta;
		if (unlikely(off < 0))
			return -EINVAL;
		cb->inner_l3_offset = off;

		off = cb->inner_l4_offset + delta;
		if (unlikely(off < 0))
			return -EINVAL;
		cb->inner_l4_offset = off;

		if (cb->inner_fragh_off) {
			off = cb->inner_fragh_off + delta;
			if (unlikely(off < 0))
				return -EINVAL;
			cb->inner_fragh_off = off;
		}
	}

	return 0;
}

#ifdef CONFIG_DEBUG_NET
/* Verify ordering/range relations between cached skb cb offsets.
 *
 * Unlike ipxlat_cb_rebase_offsets, this checks structural invariants:
 * l4 <= payload, inner_l3 >= payload, inner_l3 <= inner_l4, and fragment
 * header (when present) located inside inner L3 area before inner L4.
 */
bool ipxlat_cb_offsets_valid(const struct ipxlat_cb *cb)
{
	if (unlikely(cb->payload_off < cb->l4_off))
		return false;

	if (unlikely(cb->is_icmp_err)) {
		if (unlikely(cb->inner_l3_offset < cb->payload_off))
			return false;
		if (unlikely(cb->inner_l4_offset < cb->inner_l3_offset))
			return false;
		if (unlikely(cb->inner_fragh_off &&
			     cb->inner_fragh_off < cb->inner_l3_offset))
			return false;
		if (unlikely(cb->inner_fragh_off &&
			     cb->inner_fragh_off >= cb->inner_l4_offset))
			return false;
	}

	return true;
}
#endif

int ipxlat_v4_validate_skb(struct ipxlat_priv *ipxl, struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}

int ipxlat_v6_validate_skb(struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}
