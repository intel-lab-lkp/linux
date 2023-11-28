// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2008 - 2010 Realtek Corporation. All rights reserved.
 *
 * Contact Information: wlanfae <wlanfae@realtek.com>
 */
#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <linux/etherdevice.h>
#include "rtllib.h"
#include "rtl819x_BA.h"

static void activate_ba_entry(struct ba_record *ba, u16 time)
{
	ba->b_valid = true;
	if (time != 0)
		mod_timer(&ba->timer, jiffies + msecs_to_jiffies(time));
}

static void deactivate_ba_entry(struct rtllib_device *ieee, struct ba_record *ba)
{
	ba->b_valid = false;
	del_timer_sync(&ba->timer);
}

static u8 tx_ts_delete_ba(struct rtllib_device *ieee, struct tx_ts_record *ts)
{
	struct ba_record *admitted_ba = &ts->tx_admitted_ba_record;
	struct ba_record *pending_ba = &ts->tx_pending_ba_record;
	u8 send_del_ba = false;

	if (pending_ba->b_valid) {
		deactivate_ba_entry(ieee, pending_ba);
		send_del_ba = true;
	}

	if (admitted_ba->b_valid) {
		deactivate_ba_entry(ieee, admitted_ba);
		send_del_ba = true;
	}
	return send_del_ba;
}

static u8 rx_ts_delete_ba(struct rtllib_device *ieee, struct rx_ts_record *ts)
{
	struct ba_record *ba = &ts->rx_admitted_ba_record;
	u8			send_del_ba = false;

	if (ba->b_valid) {
		deactivate_ba_entry(ieee, ba);
		send_del_ba = true;
	}

	return send_del_ba;
}

void rtllib_reset_ba_entry(struct ba_record *ba)
{
	ba->b_valid                      = false;
	ba->ba_param_set.short_data      = 0;
	ba->ba_timeout_value             = 0;
	ba->dialog_token                 = 0;
	ba->ba_start_seq_ctrl.short_data = 0;
}

static struct sk_buff *rtllib_ADDBA(struct rtllib_device *ieee, u8 *dst,
				    struct ba_record *ba,
				    u16 status_code, u8 type)
{
	struct sk_buff *skb = NULL;
	struct ieee80211_hdr_3addr *ba_req = NULL;
	u8 *tag = NULL;
	u16 len = ieee->tx_headroom + 9;

	netdev_dbg(ieee->dev, "%s(): frame(%d) sentd to: %pM, ieee->dev:%p\n",
		   __func__, type, dst, ieee->dev);

	if (!ba) {
		netdev_warn(ieee->dev, "ba is NULL\n");
		return NULL;
	}
	skb = dev_alloc_skb(len + sizeof(struct ieee80211_hdr_3addr));
	if (!skb)
		return NULL;

	memset(skb->data, 0, sizeof(struct ieee80211_hdr_3addr));

	skb_reserve(skb, ieee->tx_headroom);

	ba_req = skb_put(skb, sizeof(struct ieee80211_hdr_3addr));

	ether_addr_copy(ba_req->addr1, dst);
	ether_addr_copy(ba_req->addr2, ieee->dev->dev_addr);

	ether_addr_copy(ba_req->addr3, ieee->current_network.bssid);
	ba_req->frame_control = cpu_to_le16(IEEE80211_STYPE_ACTION);

	tag = skb_put(skb, 9);
	*tag++ = ACT_CAT_BA;
	*tag++ = type;
	*tag++ = ba->dialog_token;

	if (type == ACT_ADDBARSP) {
		put_unaligned_le16(status_code, tag);
		tag += 2;
	}

	put_unaligned_le16(ba->ba_param_set.short_data, tag);
	tag += 2;

	put_unaligned_le16(ba->ba_timeout_value, tag);
	tag += 2;

	if (type == ACT_ADDBAREQ) {
		memcpy(tag, (u8 *)&ba->ba_start_seq_ctrl, 2);
		tag += 2;
	}

#ifdef VERBOSE_DEBUG
	print_hex_dump_bytes("%s: ", DUMP_PREFIX_NONE, skb->data,
			     __func__, skb->len);
#endif
	return skb;
}

static struct sk_buff *rtllib_DELBA(struct rtllib_device *ieee, u8 *dst,
				    struct ba_record *ba,
				    enum tr_select TxRxSelect, u16 reason_code)
{
	union delba_param_set del_ba_param_set;
	struct sk_buff *skb = NULL;
	struct ieee80211_hdr_3addr *del_ba = NULL;
	u8 *tag = NULL;
	u16 len = 6 + ieee->tx_headroom;

	if (net_ratelimit())
		netdev_dbg(ieee->dev, "%s(): reason_code(%d) sentd to: %pM\n",
			   __func__, reason_code, dst);

	memset(&del_ba_param_set, 0, 2);

	del_ba_param_set.field.initiator = (TxRxSelect == TX_DIR) ? 1 : 0;
	del_ba_param_set.field.tid	= ba->ba_param_set.field.tid;

	skb = dev_alloc_skb(len + sizeof(struct ieee80211_hdr_3addr));
	if (!skb)
		return NULL;

	skb_reserve(skb, ieee->tx_headroom);

	del_ba = skb_put(skb, sizeof(struct ieee80211_hdr_3addr));

	ether_addr_copy(del_ba->addr1, dst);
	ether_addr_copy(del_ba->addr2, ieee->dev->dev_addr);
	ether_addr_copy(del_ba->addr3, ieee->current_network.bssid);
	del_ba->frame_control = cpu_to_le16(IEEE80211_STYPE_ACTION);

	tag = skb_put(skb, 6);

	*tag++ = ACT_CAT_BA;
	*tag++ = ACT_DELBA;

	put_unaligned_le16(del_ba_param_set.short_data, tag);
	tag += 2;

	put_unaligned_le16(reason_code, tag);
	tag += 2;

#ifdef VERBOSE_DEBUG
	print_hex_dump_bytes("%s: ", DUMP_PREFIX_NONE, skb->data,
			     __func__, skb->len);
#endif
	return skb;
}

static void rtllib_send_ADDBAReq(struct rtllib_device *ieee, u8 *dst,
				 struct ba_record *ba)
{
	struct sk_buff *skb;

	skb = rtllib_ADDBA(ieee, dst, ba, 0, ACT_ADDBAREQ);

	if (skb)
		softmac_mgmt_xmit(skb, ieee);
	else
		netdev_dbg(ieee->dev, "Failed to generate ADDBAReq packet.\n");
}

static void rtllib_send_DELBA(struct rtllib_device *ieee, u8 *dst,
			      struct ba_record *ba, enum tr_select TxRxSelect,
			      u16 reason_code)
{
	struct sk_buff *skb;

	skb = rtllib_DELBA(ieee, dst, ba, TxRxSelect, reason_code);
	if (skb)
		softmac_mgmt_xmit(skb, ieee);
	else
		netdev_dbg(ieee->dev, "Failed to generate DELBA packet.\n");
}

void rtllib_ts_init_add_ba(struct rtllib_device *ieee, struct tx_ts_record *ts,
			   u8 policy, u8	overwrite_pending)
{
	struct ba_record *ba = &ts->tx_pending_ba_record;

	if (ba->b_valid && !overwrite_pending)
		return;

	deactivate_ba_entry(ieee, ba);

	ba->dialog_token++;
	ba->ba_param_set.field.amsdu_support = 0;
	ba->ba_param_set.field.ba_policy = policy;
	ba->ba_param_set.field.tid = ts->ts_common_info.tspec.ucTSID;
	ba->ba_param_set.field.buffer_size = 32;
	ba->ba_timeout_value = 0;
	ba->ba_start_seq_ctrl.field.seq_num = (ts->tx_cur_seq + 3) % 4096;

	activate_ba_entry(ba, BA_SETUP_TIMEOUT);

	rtllib_send_ADDBAReq(ieee, ts->ts_common_info.addr, ba);
}

void rtllib_ts_init_del_ba(struct rtllib_device *ieee,
			   struct ts_common_info *ts_common_info,
			   enum tr_select TxRxSelect)
{
	if (TxRxSelect == TX_DIR) {
		struct tx_ts_record *ts =
			 (struct tx_ts_record *)ts_common_info;

		if (tx_ts_delete_ba(ieee, ts))
			rtllib_send_DELBA(ieee, ts_common_info->addr,
					  (ts->tx_admitted_ba_record.b_valid) ?
					 (&ts->tx_admitted_ba_record) :
					(&ts->tx_pending_ba_record),
					 TxRxSelect, DELBA_REASON_END_BA);
	} else if (TxRxSelect == RX_DIR) {
		struct rx_ts_record *ts =
				 (struct rx_ts_record *)ts_common_info;
		if (rx_ts_delete_ba(ieee, ts))
			rtllib_send_DELBA(ieee, ts_common_info->addr,
					  &ts->rx_admitted_ba_record,
					  TxRxSelect, DELBA_REASON_END_BA);
	}
}

void rtllib_ba_setup_timeout(struct timer_list *t)
{
	struct tx_ts_record *ts = from_timer(ts, t,
					      tx_pending_ba_record.timer);

	ts->add_ba_req_in_progress = false;
	ts->add_ba_req_delayed = true;
	ts->tx_pending_ba_record.b_valid = false;
}

void rtllib_tx_ba_inact_timeout(struct timer_list *t)
{
	struct tx_ts_record *ts = from_timer(ts, t,
					      tx_admitted_ba_record.timer);
	struct rtllib_device *ieee = container_of(ts, struct rtllib_device,
				     TxTsRecord[ts->num]);
	tx_ts_delete_ba(ieee, ts);
	rtllib_send_DELBA(ieee, ts->ts_common_info.addr,
			  &ts->tx_admitted_ba_record, TX_DIR,
			  DELBA_REASON_TIMEOUT);
}

void rtllib_rx_ba_inact_timeout(struct timer_list *t)
{
	struct rx_ts_record *ts = from_timer(ts, t,
					      rx_admitted_ba_record.timer);
	struct rtllib_device *ieee = container_of(ts, struct rtllib_device,
				     RxTsRecord[ts->num]);

	rx_ts_delete_ba(ieee, ts);
	rtllib_send_DELBA(ieee, ts->ts_common_info.addr,
			  &ts->rx_admitted_ba_record, RX_DIR,
			  DELBA_REASON_TIMEOUT);
}
