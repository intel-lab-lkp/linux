/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IEEE 802.11 WLAN skb_ext definitions
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef LINUX_SKB_WIRELESS_H
#define LINUX_SKB_WIRELESS_H

#include <linux/types.h>
#include <linux/refcount.h>

/**
 * enum wireless_skb_ext_type - type of data in wireless_skb_ext
 * @WIRELESS_SKB_EXT_INVALID: uninitialized state
 * @WIRELESS_SKB_EXT_UHR_SMD: payload is @uhr_smd_ctx
 */
enum wireless_skb_ext_type {
	WIRELESS_SKB_EXT_INVALID,
	WIRELESS_SKB_EXT_UHR_SMD,
};

/**
 * struct wireless_skb_ext_smd_ctx - UHR SMD roaming context container in
 *	SKB extensions
 * @refcnt: reference count; one count per distinct skb_ext block that holds
 *	this pointer. Plain skb_clone() shares the same skb_ext block and
 *	does not increment this counter. Only skb_ext_maybe_cow() increments
 *	this counter, when a COW produces a second independent ext block copy.
 * @smd_ctx: pointer to the IEEE P802.11bn UHR SMD roaming context;
 *	must be a kmalloc-ed buffer, freed together with this container.
 */
struct wireless_skb_ext_smd_ctx {
	refcount_t refcnt;
	void *smd_ctx;
};

/**
 * struct wireless_skb_ext - SKB extension data for wireless module use
 *
 * Carried as an skb extension (SKB_EXT_WIRELESS). The union is reserved
 * for future wireless extension types; only one member is active per skb.
 *
 * @type: type of data
 * @uhr_smd_ctx: pointer to the UHR SMD context container
 */
struct wireless_skb_ext {
	enum wireless_skb_ext_type type;
	union {
		struct wireless_skb_ext_smd_ctx *uhr_smd_ctx;
	};
};

#endif /* LINUX_SKB_WIRELESS_H */
