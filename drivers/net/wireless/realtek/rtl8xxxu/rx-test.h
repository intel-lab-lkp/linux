/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RTL8XXXU_RX_TEST_H
#define RTL8XXXU_RX_TEST_H

#include <kunit/visibility.h>
#include <linux/gfp_types.h>

struct rtl8xxxu_priv;
struct rtl8xxxu_rx_urb;
struct sk_buff;
struct urb;
struct work_struct;

VISIBLE_IF_KUNIT struct rtl8xxxu_rx_urb *rtl8xxxu_alloc_rx_urb(void);
VISIBLE_IF_KUNIT struct sk_buff *rtl8xxxu_alloc_rx_skb(unsigned int size);
VISIBLE_IF_KUNIT int rtl8xxxu_rx_usb_submit(struct urb *urb, gfp_t flags);
VISIBLE_IF_KUNIT void rtl8xxxu_schedule_rx_retry(struct rtl8xxxu_priv *priv);
VISIBLE_IF_KUNIT int rtl8xxxu_alloc_rx_urbs(struct rtl8xxxu_priv *priv);
VISIBLE_IF_KUNIT int rtl8xxxu_start_rx(struct rtl8xxxu_priv *priv);
VISIBLE_IF_KUNIT int rtl8xxxu_submit_rx_urb(struct rtl8xxxu_priv *priv,
					    struct rtl8xxxu_rx_urb *rx_urb);
VISIBLE_IF_KUNIT void rtl8xxxu_free_rx_resources(struct rtl8xxxu_priv *priv);
VISIBLE_IF_KUNIT void rtl8xxxu_rx_complete(struct urb *urb);
VISIBLE_IF_KUNIT void rtl8xxxu_rx_urb_work(struct work_struct *work);
VISIBLE_IF_KUNIT void rtl8xxxu_rx_urb_retry_work(struct work_struct *work);

#endif
