// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "sec.h"
#include "tx.h"
#include "fw.h"
#include "mac.h"
#include "coex.h"
#include "ps.h"
#include "reg.h"
#include "bf.h"
#include "debug.h"
#include "wow.h"
#include "sar.h"

/* 8723BS SDIO: record a beacon/probe-resp seen from the target BSSID during
 * the pre-auth window so the join sequence (mgd_prepare_tx) can wait for it,
 * mirroring the vendor start_clnt_join(). Called from the SDIO RX path.
 */
void rtw8723bs_auth_sync_rx(struct rtw_dev *rtwdev,
			    const struct ieee80211_hdr *hdr, u32 len,
			    const struct rtw_rx_pkt_stat *pkt_stat,
			    const struct ieee80211_rx_status *rx_status)
{
	struct rtw_auth_sync *sync = &rtwdev->auth_sync;
	unsigned long flags;
	__le16 fc = hdr->frame_control;

	if (!rtw_is_8723bs(rtwdev) ||
	    test_bit(RTW_FLAG_SCANNING, rtwdev->flags) ||
	    pkt_stat->crc_err || pkt_stat->icv_err)
		return;

	if (!ieee80211_is_beacon(fc) && !ieee80211_is_probe_resp(fc))
		return;

	spin_lock_irqsave(&sync->lock, flags);
	if (sync->active && ether_addr_equal(hdr->addr3, sync->bssid)) {
		sync->seen = true;
		sync->seen_count++;
		wake_up(&sync->wait);
	}
	spin_unlock_irqrestore(&sync->lock, flags);
}
EXPORT_SYMBOL(rtw8723bs_auth_sync_rx);

/* ---- 8723BS SDIO association sequence (vendor start_clnt_join) ---- */

#define RTW8723BS_JOIN_RETRY_LIMIT		0x30
#define RTW8723BS_AUTH_SYNC_WAIT_FALLBACK_MS	120
#define RTW8723BS_AUTH_SYNC_WAIT_MIN_MS		80
#define RTW8723BS_AUTH_SYNC_WAIT_MAX_MS		160
#define RTW8723BS_ACK_PREAMBLE_SHORT		BIT(7)
#define RTW8723BS_SHORT_SLOT_TIME		9
#define RTW8723BS_LONG_SLOT_TIME		20
#define RTW8723BS_RRSR_1M			BIT(0)
#define RTW8723BS_RRSR_2M			BIT(1)
#define RTW8723BS_RRSR_5_5M			BIT(2)
#define RTW8723BS_RRSR_11M			BIT(3)
#define RTW8723BS_RRSR_6M			BIT(4)
#define RTW8723BS_RRSR_9M			BIT(5)
#define RTW8723BS_RRSR_12M			BIT(6)
#define RTW8723BS_RRSR_18M			BIT(7)
#define RTW8723BS_RRSR_24M			BIT(8)
#define RTW8723BS_RRSR_36M			BIT(9)
#define RTW8723BS_RRSR_48M			BIT(10)
#define RTW8723BS_RRSR_54M			BIT(11)
#define RTW8723BS_RRSR_CCK_RATES \
	(RTW8723BS_RRSR_1M | RTW8723BS_RRSR_2M | \
	 RTW8723BS_RRSR_5_5M | RTW8723BS_RRSR_11M)
#define RTW8723BS_RRSR_2G_FORCE			RTW8723BS_RRSR_CCK_RATES
#define RTW8723BS_RRSR_2G_ALLOW \
	(RTW8723BS_RRSR_CCK_RATES | RTW8723BS_RRSR_6M | \
	 RTW8723BS_RRSR_12M | RTW8723BS_RRSR_24M)

/* Keep the RCR at the vendor target-only state (AMF + CBSSID) across the whole
 * connect window, matching the vendor STA path. accept_all is kept only for the
 * caller's intent; both paths converge the filter to target-only.
 */
static void rtw8723bs_auth_rx_filter(struct rtw_dev *rtwdev, bool accept_all)
{
	rtwdev->hal.rcr |= BIT_AMF | BIT_CBSSID_DATA | BIT_CBSSID_BCN;
	rtwdev->hal.rcr &= ~BIT_AAP;
	rtw_write32(rtwdev, REG_RCR, rtwdev->hal.rcr);
}

static void rtw8723bs_config_sec_cfg(struct rtw_dev *rtwdev)
{
	u16 sec = rtw_read16(rtwdev, RTW_SEC_CONFIG);

	sec |= RTW_SEC_CHK_KEYID | RTW_SEC_TX_DEC_EN | RTW_SEC_RX_DEC_EN;
	rtw_write16(rtwdev, RTW_SEC_CONFIG, sec);
}

static void rtw8723bs_config_default_key_search(struct rtw_dev *rtwdev,
						bool enable)
{
	u16 sec = rtw_read16(rtwdev, RTW_SEC_CONFIG);

	if (enable)
		sec |= RTW_SEC_TX_BC_USE_DK | RTW_SEC_TX_UNI_USE_DK |
		       RTW_SEC_RX_UNI_USE_DK;
	else
		sec &= ~(RTW_SEC_TX_UNI_USE_DK | RTW_SEC_RX_UNI_USE_DK |
			 RTW_SEC_TX_BC_USE_DK | RTW_SEC_RX_BC_USE_DK);
	rtw_write16(rtwdev, RTW_SEC_CONFIG, sec);
}

static void rtw8723bs_enable_tsf_update(struct rtw_dev *rtwdev)
{
	rtw_write8_clr(rtwdev, REG_BCN_CTRL, BIT_DIS_TSF_UDT);
}

static void rtw8723bs_set_ack_preamble(struct rtw_dev *rtwdev,
				       bool short_preamble)
{
	u8 val = rtw_read8(rtwdev, REG_RRSR + 2) & ~RTW8723BS_ACK_PREAMBLE_SHORT;

	if (short_preamble)
		val |= RTW8723BS_ACK_PREAMBLE_SHORT;
	rtw_write8(rtwdev, REG_RRSR + 2, val);
}

static void rtw8723bs_set_slot_time(struct rtw_dev *rtwdev, bool short_slot)
{
	rtw_write8(rtwdev, REG_SLOT,
		   short_slot ? RTW8723BS_SHORT_SLOT_TIME :
				RTW8723BS_LONG_SLOT_TIME);
}

static u16 rtw8723bs_rrsr_from_ie_rate(u8 rate)
{
	switch (rate & 0x7f) {
	case 2:   return RTW8723BS_RRSR_1M;
	case 4:   return RTW8723BS_RRSR_2M;
	case 11:  return RTW8723BS_RRSR_5_5M;
	case 22:  return RTW8723BS_RRSR_11M;
	case 12:  return RTW8723BS_RRSR_6M;
	case 18:  return RTW8723BS_RRSR_9M;
	case 24:  return RTW8723BS_RRSR_12M;
	case 36:  return RTW8723BS_RRSR_18M;
	case 48:  return RTW8723BS_RRSR_24M;
	case 72:  return RTW8723BS_RRSR_36M;
	case 96:  return RTW8723BS_RRSR_48M;
	case 108: return RTW8723BS_RRSR_54M;
	default:  return 0;
	}
}

static void rtw8723bs_collect_basic_rates(const u8 *ie, u16 *basic_rates,
					  bool *valid)
{
	int i;

	if (!ie)
		return;

	for (i = 0; i < ie[1]; i++) {
		u16 r;

		if (!(ie[i + 2] & 0x80))
			continue;
		r = rtw8723bs_rrsr_from_ie_rate(ie[i + 2]);
		if (!r)
			continue;
		*basic_rates |= r;
		*valid = true;
	}
}

static void rtw8723bs_reset_response_rates(struct rtw_dev *rtwdev)
{
	rtw_write32(rtwdev, REG_RRSR, 0xffff1);
	rtwdev->dm_info.rrsr_val_init = 0xffff1;
}

static void rtw8723bs_apply_basic_rates(struct rtw_dev *rtwdev,
					struct ieee80211_vif *vif,
					const u8 *bssid)
{
	struct ieee80211_bss_conf *conf = &vif->bss_conf;
	struct cfg80211_bss *lookup_bss = NULL;
	struct cfg80211_bss *bss = NULL;
	bool valid = false;
	u16 basic_rates = 0;

	if (!rtw_is_8723bs(rtwdev) || vif->type != NL80211_IFTYPE_STATION)
		return;

	if (conf->bss) {
		bss = conf->bss;
	} else if (bssid && is_valid_ether_addr(bssid)) {
		lookup_bss = cfg80211_get_bss(rtwdev->hw->wiphy, NULL,
					      bssid, NULL, 0,
					      IEEE80211_BSS_TYPE_ESS,
					      IEEE80211_PRIVACY_ANY);
		bss = lookup_bss;
	}

	if (bss) {
		const u8 *rates, *ext;

		rcu_read_lock();
		rates = ieee80211_bss_get_ie(bss, WLAN_EID_SUPP_RATES);
		ext = ieee80211_bss_get_ie(bss, WLAN_EID_EXT_SUPP_RATES);
		rtw8723bs_collect_basic_rates(rates, &basic_rates, &valid);
		rtw8723bs_collect_basic_rates(ext, &basic_rates, &valid);
		rcu_read_unlock();
	}

	if (valid) {
		basic_rates |= RTW8723BS_RRSR_2G_FORCE;
		basic_rates &= RTW8723BS_RRSR_2G_ALLOW;
		rtw_write16(rtwdev, REG_RRSR, basic_rates);
		rtw_write8(rtwdev, REG_RRSR + 2,
			   rtw_read8(rtwdev, REG_RRSR + 2) & 0xf0);
		rtwdev->dm_info.rrsr_val_init = basic_rates;
	}

	if (lookup_bss)
		cfg80211_put_bss(rtwdev->hw->wiphy, lookup_bss);
}

/* Program response slot time (and, when set_preamble, the ACK preamble) from
 * the selected scan BSS capabilities; the AP capabilities are not yet in
 * bss_conf at mgd_prepare_tx() time.
 */
static void rtw8723bs_apply_bss_cap(struct rtw_dev *rtwdev,
				    struct ieee80211_vif *vif,
				    const u8 *bssid, bool set_preamble)
{
	struct ieee80211_bss_conf *conf = &vif->bss_conf;
	struct cfg80211_bss *lookup_bss = NULL;
	struct cfg80211_bss *bss = NULL;
	bool short_preamble, short_slot;
	u16 cap = 0;

	if (!rtw_is_8723bs(rtwdev) || vif->type != NL80211_IFTYPE_STATION)
		return;

	if (conf->bss) {
		bss = conf->bss;
	} else if (bssid && is_valid_ether_addr(bssid)) {
		lookup_bss = cfg80211_get_bss(rtwdev->hw->wiphy, NULL,
					      bssid, NULL, 0,
					      IEEE80211_BSS_TYPE_ESS,
					      IEEE80211_PRIVACY_ANY);
		bss = lookup_bss;
	}

	if (bss) {
		cap = bss->capability;
	} else if (conf->assoc_capability) {
		cap = conf->assoc_capability;
	} else {
		short_preamble = conf->use_short_preamble;
		short_slot = conf->use_short_slot;
		goto program;
	}
	short_preamble = !!(cap & WLAN_CAPABILITY_SHORT_PREAMBLE);
	short_slot = !!(cap & WLAN_CAPABILITY_SHORT_SLOT_TIME);

program:
	if (set_preamble)
		rtw8723bs_set_ack_preamble(rtwdev, short_preamble);
	rtw8723bs_set_slot_time(rtwdev, short_slot);

	if (lookup_bss)
		cfg80211_put_bss(rtwdev->hw->wiphy, lookup_bss);
}

static unsigned int rtw8723bs_auth_sync_wait_ms(struct ieee80211_vif *vif)
{
	u16 beacon_int = vif->bss_conf.beacon_int;
	unsigned int wait_ms;

	if (!beacon_int)
		return RTW8723BS_AUTH_SYNC_WAIT_FALLBACK_MS;

	wait_ms = DIV_ROUND_UP(beacon_int * 1024, 1000) + 20;
	return clamp_t(unsigned int, wait_ms, RTW8723BS_AUTH_SYNC_WAIT_MIN_MS,
		       RTW8723BS_AUTH_SYNC_WAIT_MAX_MS);
}

static void rtw8723bs_auth_sync_start(struct rtw_dev *rtwdev, const u8 *bssid)
{
	struct rtw_auth_sync *sync = &rtwdev->auth_sync;
	unsigned long flags;

	spin_lock_irqsave(&sync->lock, flags);
	ether_addr_copy(sync->bssid, bssid);
	sync->seen = false;
	sync->seen_count = 0;
	sync->active = true;
	spin_unlock_irqrestore(&sync->lock, flags);
}

static void rtw8723bs_auth_sync_stop(struct rtw_dev *rtwdev)
{
	struct rtw_auth_sync *sync = &rtwdev->auth_sync;
	unsigned long flags;

	spin_lock_irqsave(&sync->lock, flags);
	sync->active = false;
	spin_unlock_irqrestore(&sync->lock, flags);
}

static bool rtw8723bs_auth_sync_seen(struct rtw_dev *rtwdev)
{
	struct rtw_auth_sync *sync = &rtwdev->auth_sync;
	unsigned long flags;
	bool seen;

	spin_lock_irqsave(&sync->lock, flags);
	seen = sync->seen;
	spin_unlock_irqrestore(&sync->lock, flags);

	return seen;
}

static bool rtw8723bs_auth_sync_wait(struct rtw_dev *rtwdev,
				     unsigned int wait_ms)
{
	struct rtw_auth_sync *sync = &rtwdev->auth_sync;

	return wait_event_timeout(sync->wait, rtw8723bs_auth_sync_seen(rtwdev),
				  msecs_to_jiffies(wait_ms)) > 0;
}

static bool rtw8723bs_mgd_prepare_is_auth(struct rtw_dev *rtwdev,
					  struct ieee80211_prep_tx_info *info)
{
	return rtw_is_8723bs(rtwdev) && info &&
	       info->subtype == IEEE80211_STYPE_AUTH;
}

/* Replicate the vendor start_clnt_join() register programming right before
 * auth. Returns true for a fresh join (BSSID changed).
 */
static bool rtw8723bs_mgd_prepare_join(struct rtw_dev *rtwdev,
				       struct ieee80211_vif *vif,
				       const u8 *bssid)
{
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	bool fresh_join;
	u16 retry_limit;

	if (!is_valid_ether_addr(bssid))
		return false;

	fresh_join = !ether_addr_equal(rtwvif->bssid, bssid);

	ether_addr_copy(rtwvif->bssid, bssid);
	rtwvif->aid = 0;
	rtwvif->net_type = RTW_NET_MGD_LINKED;
	rtw_vif_port_config(rtwdev, rtwvif,
			    PORT_SET_BSSID | PORT_SET_AID | PORT_SET_NET_TYPE);

	/* Do not narrow RRSR or switch to short-preamble responses before the
	 * exchange: the whole auth/assoc runs on the init response set
	 * (0xffff1, long preamble). Only slot time is programmed early.
	 */
	rtw8723bs_apply_bss_cap(rtwdev, vif, bssid, false);

	rtw_fw_beacon_filter_config(rtwdev, false, vif);

	/* Match the vendor start_clnt_join() TX state (set_msr directly, keep
	 * BCN_CTRL / BCNQ_DL set, reassert TBTT/RESP_SIFS every join).
	 */
	rtw_write8(rtwdev, REG_BCN_CTRL,
		   BIT_DIS_TSF_UDT | BIT_EN_BCN_FUNCTION);
	rtw_write32_set(rtwdev, REG_FWHW_TXQ_CTRL, BIT_EN_BCNQ_DL);
	rtw_write8(rtwdev, REG_TBTT_PROHIBIT + 1, 0x64 & 0xff);
	rtw_write8(rtwdev, REG_TBTT_PROHIBIT + 2,
		   (rtw_read8(rtwdev, REG_TBTT_PROHIBIT + 2) & 0xf0) | (0x64 >> 8));
	rtw_write16(rtwdev, REG_RESP_SIFS_CCK, 0x0808);
	rtw_write16(rtwdev, REG_RESP_SIFS_OFDM, 0x0a0a);

	rtw_write16(rtwdev, REG_RXFLTMAP0, 0xffff);
	rtw_write16(rtwdev, REG_RXFLTMAP2, 0xffff);
	rtw8723bs_auth_rx_filter(rtwdev, true);

	retry_limit = (RTW8723BS_JOIN_RETRY_LIMIT << 8) |
		      RTW8723BS_JOIN_RETRY_LIMIT;
	rtw_write16(rtwdev, REG_RETRY_LIMIT, retry_limit);

	rtw8723bs_config_sec_cfg(rtwdev);

	return fresh_join;
}

/* The vendor sends a deauth to the target before auth to clear stale AP-side
 * state; synthesize and TX one, then let the AP settle.
 */
static void rtw8723bs_tx_pre_auth_deauth(struct rtw_dev *rtwdev,
					 struct ieee80211_vif *vif,
					 const u8 *bssid)
{
	struct ieee80211_tx_control control = {};
	struct ieee80211_tx_info *info;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;
	unsigned int frame_len, headroom;

	frame_len = sizeof(struct ieee80211_hdr_3addr) + sizeof(mgmt->u.deauth);
	headroom = rtwdev->chip->tx_pkt_desc_sz + 8;

	skb = dev_alloc_skb(headroom + frame_len);
	if (!skb)
		return;

	skb_reserve(skb, headroom);
	mgmt = skb_put_zero(skb, frame_len);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_DEAUTH);
	memcpy(mgmt->da, bssid, ETH_ALEN);
	memcpy(mgmt->sa, vif->addr, ETH_ALEN);
	memcpy(mgmt->bssid, bssid, ETH_ALEN);
	mgmt->u.deauth.reason_code = cpu_to_le16(WLAN_REASON_DEAUTH_LEAVING);

	info = IEEE80211_SKB_CB(skb);
	memset(info, 0, sizeof(*info));
	info->control.vif = vif;

	rtw_tx(rtwdev, &control, skb);
	msleep(100);
}

/* Orchestrate the pre-auth join: program the vendor join state, send the
 * pre-auth deauth, wait for a beacon from the target, then replay the
 * pre-auth coex H2Cs - once per fresh BSSID.
 */
static void rtw8723bs_mgd_prepare_auth_join(struct rtw_dev *rtwdev,
					    struct ieee80211_vif *vif,
					    struct ieee80211_prep_tx_info *info)
{
	struct rtw_vif *rtwvif;
	const u8 *bssid = NULL;
	bool fresh_join;

	if (!rtw8723bs_mgd_prepare_is_auth(rtwdev, info) || !vif ||
	    test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		return;

	rtwvif = (struct rtw_vif *)vif->drv_priv;

	if (!is_zero_ether_addr(vif->cfg.ap_addr))
		bssid = vif->cfg.ap_addr;
	else if (vif->bss_conf.bssid && !is_zero_ether_addr(vif->bss_conf.bssid))
		bssid = vif->bss_conf.bssid;

	if (!bssid)
		return;

	fresh_join = rtw8723bs_mgd_prepare_join(rtwdev, vif, bssid);

	if (fresh_join || !rtwvif->pre_auth_join_done) {
		unsigned int wait_ms = rtw8723bs_auth_sync_wait_ms(vif);

		rtw8723bs_auth_sync_start(rtwdev, bssid);
		rtw8723bs_tx_pre_auth_deauth(rtwdev, vif, bssid);
		rtw8723bs_auth_sync_wait(rtwdev, wait_ms);
		rtw8723bs_auth_sync_stop(rtwdev);
		rtwvif->pre_auth_join_done = true;
	}

	if (!rtwvif->pre_auth_h2c_sent) {
		rtw_coex_8723bs_pre_auth_h2c(rtwdev);
		rtwvif->pre_auth_h2c_sent = true;
	}
}

static void rtw_ops_tx(struct ieee80211_hw *hw,
		       struct ieee80211_tx_control *control,
		       struct sk_buff *skb)
{
	struct rtw_dev *rtwdev = hw->priv;

	if (!test_bit(RTW_FLAG_RUNNING, rtwdev->flags)) {
		ieee80211_free_txskb(hw, skb);
		return;
	}

	rtw_tx(rtwdev, control, skb);
}

static void rtw_ops_wake_tx_queue(struct ieee80211_hw *hw,
				  struct ieee80211_txq *txq)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_txq *rtwtxq = (struct rtw_txq *)txq->drv_priv;

	if (!test_bit(RTW_FLAG_RUNNING, rtwdev->flags))
		return;

	spin_lock_bh(&rtwdev->txq_lock);
	if (list_empty(&rtwtxq->list))
		list_add_tail(&rtwtxq->list, &rtwdev->txqs);
	spin_unlock_bh(&rtwdev->txq_lock);

	/* ensure to dequeue EAPOL (4/4) at the right time */
	if (txq->ac == IEEE80211_AC_VO)
		__rtw_tx_work(rtwdev);
	else
		queue_work(rtwdev->tx_wq, &rtwdev->tx_work);
}

static int rtw_ops_start(struct ieee80211_hw *hw)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret;

	mutex_lock(&rtwdev->mutex);
	ret = rtw_core_start(rtwdev);
	mutex_unlock(&rtwdev->mutex);

	return ret;
}

static void rtw_ops_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_core_stop(rtwdev);
	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret = 0;

	/* let previous ips work finish to ensure we don't leave ips twice */
	cancel_work_sync(&rtwdev->ips_work);

	mutex_lock(&rtwdev->mutex);

	rtw_leave_lps_deep(rtwdev);

	if ((changed & IEEE80211_CONF_CHANGE_IDLE) &&
	    !(hw->conf.flags & IEEE80211_CONF_IDLE)) {
		ret = rtw_leave_ips(rtwdev);
		if (ret) {
			rtw_err(rtwdev, "failed to leave idle state\n");
			goto out;
		}
	}

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
		rtw_set_channel(rtwdev);

	if ((changed & IEEE80211_CONF_CHANGE_IDLE) &&
	    (hw->conf.flags & IEEE80211_CONF_IDLE) &&
	    !test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		rtw_enter_ips(rtwdev);

out:
	mutex_unlock(&rtwdev->mutex);
	return ret;
}

static const struct rtw_vif_port rtw_vif_port[] = {
	[0] = {
		.mac_addr	= {.addr = 0x0610},
		.bssid		= {.addr = 0x0618},
		.net_type	= {.addr = 0x0100, .mask = 0x30000},
		.aid		= {.addr = 0x06a8, .mask = 0x7ff},
		.bcn_ctrl	= {.addr = 0x0550, .mask = 0xff},
	},
	[1] = {
		.mac_addr	= {.addr = 0x0700},
		.bssid		= {.addr = 0x0708},
		.net_type	= {.addr = 0x0100, .mask = 0xc0000},
		.aid		= {.addr = 0x0710, .mask = 0x7ff},
		.bcn_ctrl	= {.addr = 0x0551, .mask = 0xff},
	},
	[2] = {
		.mac_addr	= {.addr = 0x1620},
		.bssid		= {.addr = 0x1628},
		.net_type	= {.addr = 0x1100, .mask = 0x3},
		.aid		= {.addr = 0x1600, .mask = 0x7ff},
		.bcn_ctrl	= {.addr = 0x0578, .mask = 0xff},
	},
	[3] = {
		.mac_addr	= {.addr = 0x1630},
		.bssid		= {.addr = 0x1638},
		.net_type	= {.addr = 0x1100, .mask = 0xc},
		.aid		= {.addr = 0x1604, .mask = 0x7ff},
		.bcn_ctrl	= {.addr = 0x0579, .mask = 0xff},
	},
	[4] = {
		.mac_addr	= {.addr = 0x1640},
		.bssid		= {.addr = 0x1648},
		.net_type	= {.addr = 0x1100, .mask = 0x30},
		.aid		= {.addr = 0x1608, .mask = 0x7ff},
		.bcn_ctrl	= {.addr = 0x057a, .mask = 0xff},
	},
};

static int rtw_ops_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	enum rtw_net_type net_type;
	u32 config = 0;
	u8 port;
	u8 bcn_ctrl = 0;

	if (rtw_fw_feature_check(&rtwdev->fw, FW_FEATURE_BCN_FILTER))
		vif->driver_flags |= IEEE80211_VIF_BEACON_FILTER |
				     IEEE80211_VIF_SUPPORTS_CQM_RSSI;
	rtwvif->stats.tx_unicast = 0;
	rtwvif->stats.rx_unicast = 0;
	rtwvif->stats.tx_cnt = 0;
	rtwvif->stats.rx_cnt = 0;
	rtwvif->scan_req = NULL;
	memset(&rtwvif->bfee, 0, sizeof(struct rtw_bfee));
	rtw_txq_init(rtwdev, vif->txq);
	INIT_LIST_HEAD(&rtwvif->rsvd_page_list);

	mutex_lock(&rtwdev->mutex);

	rtwvif->mac_id = rtw_acquire_macid(rtwdev);
	if (rtwvif->mac_id >= RTW_MAX_MAC_ID_NUM) {
		mutex_unlock(&rtwdev->mutex);
		return -ENOSPC;
	}

	port = find_first_zero_bit(rtwdev->hw_port, RTW_PORT_NUM);
	if (port >= RTW_PORT_NUM) {
		mutex_unlock(&rtwdev->mutex);
		return -EINVAL;
	}
	set_bit(port, rtwdev->hw_port);

	rtwvif->port = port;
	rtwvif->conf = &rtw_vif_port[port];
	rtw_leave_lps_deep(rtwdev);

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		rtw_add_rsvd_page_bcn(rtwdev, rtwvif);
		net_type = RTW_NET_AP_MODE;
		bcn_ctrl = BIT_EN_BCN_FUNCTION | BIT_DIS_TSF_UDT;
		break;
	case NL80211_IFTYPE_ADHOC:
		rtw_add_rsvd_page_bcn(rtwdev, rtwvif);
		net_type = RTW_NET_AD_HOC;
		bcn_ctrl = BIT_EN_BCN_FUNCTION | BIT_DIS_TSF_UDT;
		break;
	case NL80211_IFTYPE_STATION:
		rtw_add_rsvd_page_sta(rtwdev, rtwvif);
		net_type = RTW_NET_NO_LINK;
		bcn_ctrl = BIT_EN_BCN_FUNCTION;
		break;
	default:
		WARN_ON(1);
		clear_bit(rtwvif->port, rtwdev->hw_port);
		mutex_unlock(&rtwdev->mutex);
		return -EINVAL;
	}

	ether_addr_copy(rtwvif->mac_addr, vif->addr);
	config |= PORT_SET_MAC_ADDR;
	rtwvif->net_type = net_type;
	config |= PORT_SET_NET_TYPE;
	rtwvif->bcn_ctrl = bcn_ctrl;
	config |= PORT_SET_BCN_CTRL;
	rtw_vif_port_config(rtwdev, rtwvif, config);
	rtw_core_port_switch(rtwdev, vif);
	rtw_recalc_lps(rtwdev, vif);

	mutex_unlock(&rtwdev->mutex);

	rtw_dbg(rtwdev, RTW_DBG_STATE, "start vif %pM mac_id %d on port %d\n",
		vif->addr, rtwvif->mac_id, rtwvif->port);
	return 0;
}

static void rtw_ops_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	u32 config = 0;

	rtw_dbg(rtwdev, RTW_DBG_STATE, "stop vif %pM mac_id %d on port %d\n",
		vif->addr, rtwvif->mac_id, rtwvif->port);

	mutex_lock(&rtwdev->mutex);

	rtw_leave_lps_deep(rtwdev);

	rtw_txq_cleanup(rtwdev, vif->txq);
	rtw_remove_rsvd_page(rtwdev, rtwvif);

	eth_zero_addr(rtwvif->mac_addr);
	config |= PORT_SET_MAC_ADDR;
	rtwvif->net_type = RTW_NET_NO_LINK;
	config |= PORT_SET_NET_TYPE;
	rtwvif->bcn_ctrl = 0;
	config |= PORT_SET_BCN_CTRL;
	rtw_vif_port_config(rtwdev, rtwvif, config);
	clear_bit(rtwvif->port, rtwdev->hw_port);
	rtw_release_macid(rtwdev, rtwvif->mac_id);
	rtw_recalc_lps(rtwdev, NULL);

	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_change_interface(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    enum nl80211_iftype type, bool p2p)
{
	struct rtw_dev *rtwdev = hw->priv;

	rtw_dbg(rtwdev, RTW_DBG_STATE, "change vif %pM (%d)->(%d), p2p (%d)->(%d)\n",
		vif->addr, vif->type, type, vif->p2p, p2p);

	rtw_ops_remove_interface(hw, vif);

	vif->type = type;
	vif->p2p = p2p;

	return rtw_ops_add_interface(hw, vif);
}

static void rtw_ops_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *new_flags,
				     u64 multicast)
{
	struct rtw_dev *rtwdev = hw->priv;

	*new_flags &= FIF_ALLMULTI | FIF_OTHER_BSS | FIF_FCSFAIL |
		      FIF_BCN_PRBRESP_PROMISC | FIF_CONTROL;

	mutex_lock(&rtwdev->mutex);

	rtw_leave_lps_deep(rtwdev);

	if (changed_flags & FIF_CONTROL) {
		if (*new_flags & FIF_CONTROL)
			rtw_write16(rtwdev, REG_RXFLTMAP1, 0xffff);
		else
			rtw_write16(rtwdev, REG_RXFLTMAP1, rtwdev->hal.rxfltmap1);
	}
	if (changed_flags & FIF_ALLMULTI) {
		if (*new_flags & FIF_ALLMULTI)
			rtwdev->hal.rcr |= BIT_AM;
		else
			rtwdev->hal.rcr &= ~(BIT_AM);
	}
	if (changed_flags & FIF_FCSFAIL) {
		if (*new_flags & FIF_FCSFAIL)
			rtwdev->hal.rcr |= BIT_ACRC32;
		else
			rtwdev->hal.rcr &= ~(BIT_ACRC32);
	}
	if (changed_flags & FIF_OTHER_BSS) {
		if (*new_flags & FIF_OTHER_BSS)
			rtwdev->hal.rcr |= BIT_AAP;
		else
			rtwdev->hal.rcr &= ~(BIT_AAP);
	}
	if (changed_flags & FIF_BCN_PRBRESP_PROMISC) {
		if (*new_flags & FIF_BCN_PRBRESP_PROMISC)
			rtwdev->hal.rcr &= ~(BIT_CBSSID_BCN | BIT_CBSSID_DATA);
		else
			rtwdev->hal.rcr |= BIT_CBSSID_BCN;
	}

	rtw_dbg(rtwdev, RTW_DBG_RX,
		"config rx filter, changed=0x%08x, new=0x%08x, rcr=0x%08x\n",
		changed_flags, *new_flags, rtwdev->hal.rcr);

	rtw_write32(rtwdev, REG_RCR, rtwdev->hal.rcr);

	mutex_unlock(&rtwdev->mutex);
}

/* Only have one group of EDCA parameters now */
static const u32 ac_to_edca_param[IEEE80211_NUM_ACS] = {
	[IEEE80211_AC_VO] = REG_EDCA_VO_PARAM,
	[IEEE80211_AC_VI] = REG_EDCA_VI_PARAM,
	[IEEE80211_AC_BE] = REG_EDCA_BE_PARAM,
	[IEEE80211_AC_BK] = REG_EDCA_BK_PARAM,
};

static u8 rtw_aifsn_to_aifs(struct rtw_dev *rtwdev,
			    struct rtw_vif *rtwvif, u8 aifsn)
{
	struct ieee80211_vif *vif = rtwvif_to_vif(rtwvif);
	u8 slot_time;
	u8 sifs;

	slot_time = vif->bss_conf.use_short_slot ? 9 : 20;
	sifs = rtwdev->hal.current_band_type == RTW_BAND_5G ? 16 : 10;

	return aifsn * slot_time + sifs;
}

static void __rtw_conf_tx(struct rtw_dev *rtwdev,
			  struct rtw_vif *rtwvif, u16 ac)
{
	struct ieee80211_tx_queue_params *params = &rtwvif->tx_params[ac];
	u32 edca_param = ac_to_edca_param[ac];
	u8 ecw_max, ecw_min;
	u8 aifs;

	/* 2^ecw - 1 = cw; ecw = log2(cw + 1) */
	ecw_max = ilog2(params->cw_max + 1);
	ecw_min = ilog2(params->cw_min + 1);
	aifs = rtw_aifsn_to_aifs(rtwdev, rtwvif, params->aifs);
	rtw_write32_mask(rtwdev, edca_param, BIT_MASK_TXOP_LMT, params->txop);
	rtw_write32_mask(rtwdev, edca_param, BIT_MASK_CWMAX, ecw_max);
	rtw_write32_mask(rtwdev, edca_param, BIT_MASK_CWMIN, ecw_min);
	rtw_write32_mask(rtwdev, edca_param, BIT_MASK_AIFS, aifs);
}

static void rtw_conf_tx(struct rtw_dev *rtwdev,
			struct rtw_vif *rtwvif)
{
	u16 ac;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++)
		__rtw_conf_tx(rtwdev, rtwvif, ac);
}

static void rtw_ops_bss_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *conf,
				     u64 changed)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;
	struct rtw_coex *coex = &rtwdev->coex;
	struct rtw_coex_stat *coex_stat = &coex->stat;
	u32 config = 0;

	mutex_lock(&rtwdev->mutex);

	rtw_leave_lps_deep(rtwdev);

	if (changed & BSS_CHANGED_ASSOC) {
		rtw_vif_assoc_changed(rtwvif, conf);
		if (vif->cfg.assoc) {
			if (rtw_is_8723bs(rtwdev) &&
			    vif->type == NL80211_IFTYPE_STATION) {
				rtw8723bs_auth_rx_filter(rtwdev, false);
				rtw8723bs_apply_bss_cap(rtwdev, vif, NULL, true);
				rtw8723bs_apply_basic_rates(rtwdev, vif, NULL);
				rtw8723bs_enable_tsf_update(rtwdev);
				/* Vendor mlmeext_joinbss sends MACID_CFG before
				 * MEDIA_STATUS_RPT, then WL_CH_INFO.
				 */
				rtw_fw_macid_cfg(rtwdev, rtwvif->mac_id,
						 1, 0, 1, 0x0ff015);
				if (!rtwvif->fw_media_connected) {
					rtw_fw_media_status_report(rtwdev,
								   rtwvif->mac_id,
								   true);
					rtwvif->fw_media_connected = true;
				}
				rtw_fw_send_wl_ch_info(rtwdev,
						       rtwdev->hal.current_channel,
						       rtwdev->hal.current_band_width);
			}

			rtw_coex_connect_notify(rtwdev, COEX_ASSOCIATE_FINISH);

			rtw_fw_download_rsvd_page(rtwdev);
			rtw_send_rsvd_page_h2c(rtwdev);
			rtw_fw_default_port(rtwdev, rtwvif);
			rtw_coex_media_status_notify(rtwdev, vif->cfg.assoc);
			if (rtw_bf_support)
				rtw_bf_assoc(rtwdev, vif, conf);

			rtw_set_ampdu_factor(rtwdev, vif, conf);

			rtw_fw_beacon_filter_config(rtwdev, true, vif);
		} else {
			rtw_leave_lps(rtwdev);
			rtw_bf_disassoc(rtwdev, vif, conf);
			/* Abort ongoing scan if cancel_scan isn't issued
			 * when disconnected by peer
			 */
			if (test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
				rtw_hw_scan_abort(rtwdev);

			if (rtw_is_8723bs(rtwdev) &&
			    vif->type == NL80211_IFTYPE_STATION) {
				rtw8723bs_auth_rx_filter(rtwdev, false);
				rtw8723bs_reset_response_rates(rtwdev);
				rtwvif->pre_auth_h2c_sent = false;
				rtwvif->pre_auth_join_done = false;
			}
		}

		config |= PORT_SET_NET_TYPE;
		config |= PORT_SET_AID;
	}

	if (changed & BSS_CHANGED_BSSID) {
		bool bssid_cleared = is_zero_ether_addr(conf->bssid);
		bool bssid_changed = !ether_addr_equal(rtwvif->bssid,
						       conf->bssid);

		if (rtw_is_8723bs(rtwdev) &&
		    vif->type == NL80211_IFTYPE_STATION && bssid_changed) {
			rtwvif->pre_auth_h2c_sent = false;
			rtwvif->pre_auth_join_done = false;
		}
		ether_addr_copy(rtwvif->bssid, conf->bssid);
		config |= PORT_SET_BSSID;
		if (rtw_is_8723bs(rtwdev) &&
		    vif->type == NL80211_IFTYPE_STATION && bssid_cleared) {
			rtwvif->aid = 0;
			rtwvif->net_type = RTW_NET_NO_LINK;
			config |= PORT_SET_NET_TYPE | PORT_SET_AID;
			rtw_write8(rtwdev, REG_BCN_CTRL,
				   BIT_DIS_TSF_UDT | BIT_EN_BCN_FUNCTION |
				   BIT_DIS_ATIM);
		}
		if (!rtw_core_check_sta_active(rtwdev))
			rtw_clear_op_chan(rtwdev);
		else
			rtw_store_op_chan(rtwdev, true);
	}

	if (changed & BSS_CHANGED_BEACON_INT) {
		if (ieee80211_vif_type_p2p(vif) == NL80211_IFTYPE_STATION)
			coex_stat->wl_beacon_interval = conf->beacon_int;
	}

	if (changed & BSS_CHANGED_BEACON) {
		rtw_set_dtim_period(rtwdev, conf->dtim_period);
		rtw_fw_download_rsvd_page(rtwdev);
		rtw_send_rsvd_page_h2c(rtwdev);
	}

	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		if (conf->enable_beacon)
			rtw_write32_set(rtwdev, REG_FWHW_TXQ_CTRL,
					BIT_EN_BCNQ_DL);
		else
			rtw_write32_clr(rtwdev, REG_FWHW_TXQ_CTRL,
					BIT_EN_BCNQ_DL);
	}
	if (changed & BSS_CHANGED_CQM)
		rtw_fw_beacon_filter_config(rtwdev, true, vif);

	if (changed & BSS_CHANGED_MU_GROUPS)
		rtw_chip_set_gid_table(rtwdev, vif, conf);

	if (changed & BSS_CHANGED_ERP_PREAMBLE &&
	    rtw_is_8723bs(rtwdev) &&
	    vif->type == NL80211_IFTYPE_STATION)
		rtw8723bs_set_ack_preamble(rtwdev, conf->use_short_preamble);

	if (changed & BSS_CHANGED_ERP_SLOT) {
		if (rtw_is_8723bs(rtwdev) &&
		    vif->type == NL80211_IFTYPE_STATION)
			rtw8723bs_set_slot_time(rtwdev, conf->use_short_slot);
		rtw_conf_tx(rtwdev, rtwvif);
	}

	if (changed & BSS_CHANGED_PS)
		rtw_recalc_lps(rtwdev, NULL);

	rtw_vif_port_config(rtwdev, rtwvif, config);

	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_start_ap(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif,
			    struct ieee80211_bss_conf *link_conf)
{
	struct rtw_dev *rtwdev = hw->priv;
	const struct rtw_chip_info *chip = rtwdev->chip;

	mutex_lock(&rtwdev->mutex);
	rtw_write32_set(rtwdev, REG_TCR, BIT_TCR_UPDATE_HGQMD);
	rtwdev->ap_active = true;
	rtw_store_op_chan(rtwdev, true);
	chip->ops->phy_calibration(rtwdev);
	mutex_unlock(&rtwdev->mutex);

	return 0;
}

static void rtw_ops_stop_ap(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif,
			    struct ieee80211_bss_conf *link_conf)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_write32_clr(rtwdev, REG_TCR, BIT_TCR_UPDATE_HGQMD);
	rtwdev->ap_active = false;
	if (!rtw_core_check_sta_active(rtwdev))
		rtw_clear_op_chan(rtwdev);
	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_conf_tx(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif,
			   unsigned int link_id, u16 ac,
			   const struct ieee80211_tx_queue_params *params)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

	mutex_lock(&rtwdev->mutex);

	rtw_leave_lps_deep(rtwdev);

	rtwvif->tx_params[ac] = *params;
	__rtw_conf_tx(rtwdev, rtwvif, ac);

	mutex_unlock(&rtwdev->mutex);

	return 0;
}

static int rtw_ops_sta_add(struct ieee80211_hw *hw,
			   struct ieee80211_vif *vif,
			   struct ieee80211_sta *sta)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret = 0;

	mutex_lock(&rtwdev->mutex);
	ret = rtw_sta_add(rtwdev, sta, vif);
	mutex_unlock(&rtwdev->mutex);

	return ret;
}

static int rtw_ops_sta_remove(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_sta *sta)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_fw_beacon_filter_config(rtwdev, false, vif);
	rtw_sta_remove(rtwdev, sta, true);
	mutex_unlock(&rtwdev->mutex);

	return 0;
}

static int rtw_ops_set_tim(struct ieee80211_hw *hw, struct ieee80211_sta *sta,
			   bool set)
{
	struct rtw_dev *rtwdev = hw->priv;

	ieee80211_queue_work(hw, &rtwdev->update_beacon_work);

	return 0;
}

static int rtw_ops_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
			   struct ieee80211_vif *vif, struct ieee80211_sta *sta,
			   struct ieee80211_key_conf *key)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_sec_desc *sec = &rtwdev->sec;
	u8 hw_key_type;
	u8 hw_key_idx;
	int ret = 0;

	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		hw_key_type = RTW_CAM_WEP40;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		hw_key_type = RTW_CAM_WEP104;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		hw_key_type = RTW_CAM_TKIP;
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		hw_key_type = RTW_CAM_AES;
		key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
		break;
	case WLAN_CIPHER_SUITE_AES_CMAC:
	case WLAN_CIPHER_SUITE_BIP_CMAC_256:
	case WLAN_CIPHER_SUITE_BIP_GMAC_128:
	case WLAN_CIPHER_SUITE_BIP_GMAC_256:
	case WLAN_CIPHER_SUITE_CCMP_256:
	case WLAN_CIPHER_SUITE_GCMP:
	case WLAN_CIPHER_SUITE_GCMP_256:
		/* suppress error messages */
		return -EOPNOTSUPP;
	default:
		return -ENOTSUPP;
	}

	mutex_lock(&rtwdev->mutex);

	rtw_leave_lps_deep(rtwdev);

	if (key->flags & IEEE80211_KEY_FLAG_PAIRWISE) {
		hw_key_idx = rtw_sec_get_free_cam(sec);
	} else {
		/* multiple interfaces? */
		hw_key_idx = key->keyidx;
	}

	if (hw_key_idx > sec->total_cam_num) {
		ret = -ENOSPC;
		goto out;
	}

	switch (cmd) {
	case SET_KEY:
		/* need sw generated IV */
		key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
		key->hw_key_idx = hw_key_idx;
		rtw_sec_write_cam(rtwdev, sec, sta, key,
				  hw_key_type, hw_key_idx);
		if (rtw_is_8723bs(rtwdev) && vif &&
		    vif->type == NL80211_IFTYPE_STATION &&
		    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
			rtw8723bs_config_default_key_search(rtwdev, true);
		break;
	case DISABLE_KEY:
		rtw_hci_flush_all_queues(rtwdev, false);
		rtw_mac_flush_all_queues(rtwdev, false);
		rtw_sec_clear_cam(rtwdev, sec, key->hw_key_idx);
		if (rtw_is_8723bs(rtwdev) && vif &&
		    vif->type == NL80211_IFTYPE_STATION &&
		    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
			rtw8723bs_config_default_key_search(rtwdev, false);
		break;
	}

	/* download new cam settings for PG to backup */
	if (rtw_get_lps_deep_mode(rtwdev) == LPS_DEEP_MODE_PG)
		rtw_fw_download_rsvd_page(rtwdev);

out:
	mutex_unlock(&rtwdev->mutex);

	return ret;
}

static int rtw_ops_ampdu_action(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_ampdu_params *params)
{
	struct ieee80211_sta *sta = params->sta;
	u16 tid = params->tid;
	struct ieee80211_txq *txq = sta->txq[tid];
	struct rtw_txq *rtwtxq = (struct rtw_txq *)txq->drv_priv;

	switch (params->action) {
	case IEEE80211_AMPDU_TX_START:
		return IEEE80211_AMPDU_TX_START_IMMEDIATE;
	case IEEE80211_AMPDU_TX_STOP_CONT:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		clear_bit(RTW_TXQ_AMPDU, &rtwtxq->flags);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		set_bit(RTW_TXQ_AMPDU, &rtwtxq->flags);
		break;
	case IEEE80211_AMPDU_RX_START:
	case IEEE80211_AMPDU_RX_STOP:
		break;
	default:
		WARN_ON(1);
		return -ENOTSUPP;
	}

	return 0;
}

static bool rtw_ops_can_aggregate_in_amsdu(struct ieee80211_hw *hw,
					   struct sk_buff *head,
					   struct sk_buff *skb)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_hal *hal = &rtwdev->hal;

	/* we don't want to enable TX AMSDU on 2.4G */
	if (hal->current_band_type == RTW_BAND_2G)
		return false;

	return true;
}

static void rtw_ops_sw_scan_start(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  const u8 *mac_addr)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_vif *rtwvif = (struct rtw_vif *)vif->drv_priv;

	mutex_lock(&rtwdev->mutex);
	rtw_core_scan_start(rtwdev, rtwvif, mac_addr, false);
	mutex_unlock(&rtwdev->mutex);
}

static void rtw_ops_sw_scan_complete(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_core_scan_complete(rtwdev, vif, false);
	mutex_unlock(&rtwdev->mutex);
}

static void rtw_ops_mgd_prepare_tx(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_prep_tx_info *info)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_leave_lps_deep(rtwdev);

	if (rtw_is_8723bs(rtwdev)) {
		/* Wake from soft IPS and run the vendor join sequence. The RFK
		 * is handled by the once-only power-on IQK plus the ps.c
		 * post-IPS RF-bus recovery, not a fresh calibration here.
		 */
		if (rtw_leave_ips(rtwdev)) {
			rtw_err(rtwdev, "failed to leave idle state for mgd tx\n");
			goto out;
		}
		rtw_coex_connect_notify(rtwdev, COEX_ASSOCIATE_START);
		rtw8723bs_mgd_prepare_auth_join(rtwdev, vif, info);
	} else {
		rtw_coex_connect_notify(rtwdev, COEX_ASSOCIATE_START);
		rtw_chip_prepare_tx(rtwdev);
	}
out:
	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx,
				     u32 value)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtwdev->rts_threshold = value;
	mutex_unlock(&rtwdev->mutex);

	return 0;
}

static void rtw_ops_sta_statistics(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta,
				   struct station_info *sinfo)
{
	struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;

	sinfo->txrate = si->ra_report.txrate;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
}

static void rtw_ops_flush(struct ieee80211_hw *hw,
			  struct ieee80211_vif *vif,
			  u32 queues, bool drop)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_leave_lps_deep(rtwdev);

	rtw_hci_flush_queues(rtwdev, queues, drop);
	rtw_mac_flush_queues(rtwdev, queues, drop);
	mutex_unlock(&rtwdev->mutex);
}

struct rtw_iter_bitrate_mask_data {
	struct rtw_dev *rtwdev;
	struct ieee80211_vif *vif;
	const struct cfg80211_bitrate_mask *mask;
};

static void rtw_ra_mask_info_update_iter(void *data, struct ieee80211_sta *sta)
{
	struct rtw_iter_bitrate_mask_data *br_data = data;
	struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;

	if (si->vif != br_data->vif)
		return;

	/* free previous mask setting */
	kfree(si->mask);
	si->mask = kmemdup(br_data->mask, sizeof(struct cfg80211_bitrate_mask),
			   GFP_ATOMIC);
	if (!si->mask) {
		si->use_cfg_mask = false;
		return;
	}

	si->use_cfg_mask = true;
	rtw_update_sta_info(br_data->rtwdev, si, true);
}

static void rtw_ra_mask_info_update(struct rtw_dev *rtwdev,
				    struct ieee80211_vif *vif,
				    const struct cfg80211_bitrate_mask *mask)
{
	struct rtw_iter_bitrate_mask_data br_data;

	br_data.rtwdev = rtwdev;
	br_data.vif = vif;
	br_data.mask = mask;
	rtw_iterate_stas(rtwdev, rtw_ra_mask_info_update_iter, &br_data);
}

static int rtw_ops_set_bitrate_mask(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    const struct cfg80211_bitrate_mask *mask)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_ra_mask_info_update(rtwdev, vif, mask);
	mutex_unlock(&rtwdev->mutex);

	return 0;
}

static int rtw_ops_set_antenna(struct ieee80211_hw *hw,
			       int radio_idx,
			       u32 tx_antenna,
			       u32 rx_antenna)
{
	struct rtw_dev *rtwdev = hw->priv;
	const struct rtw_chip_info *chip = rtwdev->chip;
	int ret;

	if (!chip->ops->set_antenna)
		return -EOPNOTSUPP;

	mutex_lock(&rtwdev->mutex);
	ret = chip->ops->set_antenna(rtwdev, -1, tx_antenna, rx_antenna);
	mutex_unlock(&rtwdev->mutex);

	return ret;
}

static int rtw_ops_get_antenna(struct ieee80211_hw *hw,
			       int radio_idx,
			       u32 *tx_antenna,
			       u32 *rx_antenna)
{
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_hal *hal = &rtwdev->hal;

	*tx_antenna = hal->antenna_tx;
	*rx_antenna = hal->antenna_rx;

	return 0;
}

#ifdef CONFIG_PM
static int rtw_ops_suspend(struct ieee80211_hw *hw,
			   struct cfg80211_wowlan *wowlan)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret;

	mutex_lock(&rtwdev->mutex);
	ret = rtw_wow_suspend(rtwdev, wowlan);
	if (ret)
		rtw_err(rtwdev, "failed to suspend for wow %d\n", ret);
	mutex_unlock(&rtwdev->mutex);

	return ret ? 1 : 0;
}

static int rtw_ops_resume(struct ieee80211_hw *hw)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret;

	mutex_lock(&rtwdev->mutex);
	ret = rtw_wow_resume(rtwdev);
	if (ret)
		rtw_err(rtwdev, "failed to resume for wow %d\n", ret);
	mutex_unlock(&rtwdev->mutex);

	return ret ? 1 : 0;
}

static void rtw_ops_set_wakeup(struct ieee80211_hw *hw, bool enabled)
{
	struct rtw_dev *rtwdev = hw->priv;

	device_set_wakeup_enable(rtwdev->dev, enabled);
}
#endif

static void rtw_reconfig_complete(struct ieee80211_hw *hw,
				  enum ieee80211_reconfig_type reconfig_type)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	if (reconfig_type == IEEE80211_RECONFIG_TYPE_RESTART)
		clear_bit(RTW_FLAG_RESTARTING, rtwdev->flags);
	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			   struct ieee80211_scan_request *req)
{
	struct rtw_dev *rtwdev = hw->priv;
	int ret;

	if (!rtw_fw_feature_check(&rtwdev->fw, FW_FEATURE_SCAN_OFFLOAD))
		return 1;

	if (test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		return -EBUSY;

	mutex_lock(&rtwdev->mutex);
	rtw_hw_scan_start(rtwdev, vif, req);
	ret = rtw_hw_scan_offload(rtwdev, vif, true);
	if (ret) {
		rtw_hw_scan_abort(rtwdev);
		rtw_err(rtwdev, "HW scan failed with status: %d\n", ret);
	}
	mutex_unlock(&rtwdev->mutex);

	return ret;
}

static void rtw_ops_cancel_hw_scan(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif)
{
	struct rtw_dev *rtwdev = hw->priv;

	if (!rtw_fw_feature_check(&rtwdev->fw, FW_FEATURE_SCAN_OFFLOAD))
		return;

	if (!test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		return;

	mutex_lock(&rtwdev->mutex);
	rtw_hw_scan_abort(rtwdev);
	mutex_unlock(&rtwdev->mutex);
}

static int rtw_ops_set_sar_specs(struct ieee80211_hw *hw,
				 const struct cfg80211_sar_specs *sar)
{
	struct rtw_dev *rtwdev = hw->priv;

	mutex_lock(&rtwdev->mutex);
	rtw_set_sar_specs(rtwdev, sar);
	mutex_unlock(&rtwdev->mutex);

	return 0;
}

static void rtw_ops_sta_rc_update(struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  struct ieee80211_link_sta *link_sta,
				  u32 changed)
{
	struct ieee80211_sta *sta = link_sta->sta;
	struct rtw_dev *rtwdev = hw->priv;
	struct rtw_sta_info *si = (struct rtw_sta_info *)sta->drv_priv;

	if (changed & IEEE80211_RC_BW_CHANGED)
		ieee80211_queue_work(rtwdev->hw, &si->rc_work);
}

const struct ieee80211_ops rtw_ops = {
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
	.tx			= rtw_ops_tx,
	.wake_tx_queue		= rtw_ops_wake_tx_queue,
	.start			= rtw_ops_start,
	.stop			= rtw_ops_stop,
	.config			= rtw_ops_config,
	.add_interface		= rtw_ops_add_interface,
	.remove_interface	= rtw_ops_remove_interface,
	.change_interface	= rtw_ops_change_interface,
	.configure_filter	= rtw_ops_configure_filter,
	.bss_info_changed	= rtw_ops_bss_info_changed,
	.start_ap		= rtw_ops_start_ap,
	.stop_ap		= rtw_ops_stop_ap,
	.conf_tx		= rtw_ops_conf_tx,
	.sta_add		= rtw_ops_sta_add,
	.sta_remove		= rtw_ops_sta_remove,
	.set_tim		= rtw_ops_set_tim,
	.set_key		= rtw_ops_set_key,
	.ampdu_action		= rtw_ops_ampdu_action,
	.can_aggregate_in_amsdu	= rtw_ops_can_aggregate_in_amsdu,
	.sw_scan_start		= rtw_ops_sw_scan_start,
	.sw_scan_complete	= rtw_ops_sw_scan_complete,
	.mgd_prepare_tx		= rtw_ops_mgd_prepare_tx,
	.set_rts_threshold	= rtw_ops_set_rts_threshold,
	.sta_statistics		= rtw_ops_sta_statistics,
	.flush			= rtw_ops_flush,
	.set_bitrate_mask	= rtw_ops_set_bitrate_mask,
	.set_antenna		= rtw_ops_set_antenna,
	.get_antenna		= rtw_ops_get_antenna,
	.reconfig_complete	= rtw_reconfig_complete,
	.hw_scan		= rtw_ops_hw_scan,
	.cancel_hw_scan		= rtw_ops_cancel_hw_scan,
	.link_sta_rc_update	= rtw_ops_sta_rc_update,
	.set_sar_specs          = rtw_ops_set_sar_specs,
#ifdef CONFIG_PM
	.suspend		= rtw_ops_suspend,
	.resume			= rtw_ops_resume,
	.set_wakeup		= rtw_ops_set_wakeup,
#endif
};
EXPORT_SYMBOL(rtw_ops);
