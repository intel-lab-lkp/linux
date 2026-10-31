/* SPDX-License-Identifier: (GPL-2.0 OR MIT)
 * Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2021 Google, Inc.
 */

#ifndef _GVE_UTILS_H
#define _GVE_UTILS_H

#include <linux/etherdevice.h>

#include "gve.h"

bool gve_tx_was_added_to_block(struct gve_priv *priv, int queue_idx);
void gve_tx_remove_from_block(struct gve_priv *priv, int queue_idx);
void gve_tx_add_to_block(struct gve_priv *priv, int queue_idx);

bool gve_rx_was_added_to_block(struct gve_priv *priv, int queue_idx);
void gve_rx_remove_from_block(struct gve_priv *priv, int queue_idx);
void gve_rx_add_to_block(struct gve_priv *priv, int queue_idx);

struct sk_buff *gve_rx_copy_data(struct net_device *dev, struct napi_struct *napi,
				 u8 *data, u16 len);

struct sk_buff *gve_rx_copy(struct net_device *dev, struct napi_struct *napi,
			    struct gve_rx_slot_page_info *page_info, u16 len);

/* Up-front references gve holds on an RX page, so handing a buffer to the
 * stack is a pagecnt_bias decrement, not a get_page(). Keep it in a u16 so
 * the 32-bit page refcount keeps the headroom every page needs below INT_MAX.
 */
#define GVE_PAGECNT_BIAS USHRT_MAX

/* Decrement pagecnt_bias. Top it up by GVE_PAGECNT_BIAS if it reached zero. */
void gve_dec_pagecnt_bias(struct gve_rx_slot_page_info *page_info);

void gve_add_napi(struct gve_priv *priv, int ntfy_idx,
		  int (*gve_poll)(struct napi_struct *, int));
void gve_remove_napi(struct gve_priv *priv, int ntfy_idx);
#endif /* _GVE_UTILS_H */

