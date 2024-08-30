/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * VXLAN drop reason list.
 */

#ifndef VXLAN_DROP_H
#define VXLAN_DROP_H
#include <linux/skbuff.h>
#include <net/dropreason.h>

#define VXLAN_DROP_REASONS(R)			\
	R(VXLAN_DROP_INVALID_SMAC)		\
	R(VXLAN_DROP_ENTRY_EXISTS)		\
	R(VXLAN_DROP_INVALID_HDR)		\
	R(VXLAN_DROP_VNI_NOT_FOUND)		\
	R(VXLAN_DROP_NO_REMOTE)			\
	/* deliberate comment for trailing \ */

enum vxlan_drop_reason {
	__VXLAN_DROP_REASON = SKB_DROP_REASON_SUBSYS_VXLAN <<
				SKB_DROP_REASON_SUBSYS_SHIFT,
	/** @VXLAN_DROP_INVALID_SMAC: source mac is invalid */
	VXLAN_DROP_INVALID_SMAC,
	/**
	 * @VXLAN_DROP_ENTRY_EXISTS: trying to migrate a static entry or
	 * one pointing to a nexthop
	 */
	VXLAN_DROP_ENTRY_EXISTS,
	/**
	 * @VXLAN_DROP_INVALID_HDR: the vxlan header is invalid, such as:
	 * 1) the reserved fields are not zero
	 * 2) the "I" flag is not set
	 */
	VXLAN_DROP_INVALID_HDR,
	/** @VXLAN_DROP_VNI_NOT_FOUND: no vxlan device found for the vni */
	VXLAN_DROP_VNI_NOT_FOUND,
	/** @VXLAN_DROP_NO_REMOTE: no remote found to transmit the packet */
	VXLAN_DROP_NO_REMOTE,
};

static inline void
vxlan_kfree_skb(struct sk_buff *skb, enum vxlan_drop_reason reason)
{
	kfree_skb_reason(skb, (u32)reason);
}

#endif /* VXLAN_DROP_H */
