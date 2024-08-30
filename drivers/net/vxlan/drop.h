/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VXLAN drop reason list.
 */

#ifndef VXLAN_DROP_H
#define VXLAN_DROP_H
#include <linux/skbuff.h>
#include <net/dropreason.h>

#define VXLAN_DROP_REASONS(R)			\
	/* deliberate comment for trailing \ */

enum vxlan_drop_reason {
	__VXLAN_DROP_REASON = SKB_DROP_REASON_SUBSYS_VXLAN <<
				SKB_DROP_REASON_SUBSYS_SHIFT,
};

static inline void
vxlan_kfree_skb(struct sk_buff *skb, enum vxlan_drop_reason reason)
{
	kfree_skb_reason(skb, (u32)reason);
}

#endif /* VXLAN_DROP_H */
