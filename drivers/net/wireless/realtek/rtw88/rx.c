// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "mac.h"
#include "rx.h"
#include "ps.h"
#include "debug.h"
#include "fw.h"

void rtw_rx_stats(struct rtw_dev *rtwdev, struct ieee80211_vif *vif,
		  struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr;
	struct rtw_vif *rtwvif;

	hdr = (struct ieee80211_hdr *)skb->data;

	if (!ieee80211_is_data(hdr->frame_control))
		return;

	if (!is_broadcast_ether_addr(hdr->addr1) &&
	    !is_multicast_ether_addr(hdr->addr1)) {
		rtwdev->stats.rx_unicast += skb->len;
		rtwdev->stats.rx_cnt++;
		if (vif) {
			rtwvif = (struct rtw_vif *)vif->drv_priv;
			rtwvif->stats.rx_unicast += skb->len;
			rtwvif->stats.rx_cnt++;
		}
	}
}
EXPORT_SYMBOL(rtw_rx_stats);

struct rtw_rx_addr_match_data {
	struct rtw_dev *rtwdev;
	struct ieee80211_hdr *hdr;
	struct rtw_rx_pkt_stat *pkt_stat;
	u8 *bssid;
};

static void rtw_rx_phy_stat(struct rtw_dev *rtwdev,
			    struct rtw_rx_pkt_stat *pkt_stat,
			    struct ieee80211_hdr *hdr)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	struct rtw_pkt_count *cur_pkt_cnt = &dm_info->cur_pkt_count;
	u8 rate_ss, rate_ss_evm, evm_id;
	u8 i, idx;

	dm_info->curr_rx_rate = pkt_stat->rate;

	if (ieee80211_is_beacon(hdr->frame_control))
		cur_pkt_cnt->num_bcn_pkt++;

	switch (pkt_stat->rate) {
	case DESC_RATE1M...DESC_RATE11M:
		goto pkt_num;
	case DESC_RATE6M...DESC_RATE54M:
		rate_ss = 0;
		rate_ss_evm = 1;
		evm_id = RTW_EVM_OFDM;
		break;
	case DESC_RATEMCS0...DESC_RATEMCS7:
	case DESC_RATEVHT1SS_MCS0...DESC_RATEVHT1SS_MCS9:
		rate_ss = 1;
		rate_ss_evm = 1;
		evm_id = RTW_EVM_1SS;
		break;
	case DESC_RATEMCS8...DESC_RATEMCS15:
	case DESC_RATEVHT2SS_MCS0...DESC_RATEVHT2SS_MCS9:
		rate_ss = 2;
		rate_ss_evm = 2;
		evm_id = RTW_EVM_2SS_A;
		break;
	case DESC_RATEMCS16...DESC_RATEMCS23:
	case DESC_RATEVHT3SS_MCS0...DESC_RATEVHT3SS_MCS9:
		rate_ss = 3;
		rate_ss_evm = 3;
		evm_id = RTW_EVM_3SS_A;
		break;
	default:
		rtw_warn(rtwdev, "unknown pkt rate = %d\n", pkt_stat->rate);
		return;
	}

	for (i = 0; i < rate_ss_evm; i++) {
		idx = evm_id + i;
		ewma_evm_add(&dm_info->ewma_evm[idx],
			     dm_info->rx_evm_dbm[i]);
	}

	for (i = 0; i < rtwdev->hal.rf_path_num; i++) {
		idx = RTW_SNR_OFDM_A + 4 * rate_ss + i;
		ewma_snr_add(&dm_info->ewma_snr[idx],
			     dm_info->rx_snr[i]);
	}
pkt_num:
	cur_pkt_cnt->num_qry_pkt[pkt_stat->rate]++;
}

static void rtw_rx_addr_match_iter(void *data, u8 *mac,
				   struct ieee80211_vif *vif)
{
	struct rtw_rx_addr_match_data *iter_data = data;
	struct ieee80211_sta *sta;
	struct ieee80211_hdr *hdr = iter_data->hdr;
	struct rtw_dev *rtwdev = iter_data->rtwdev;
	struct rtw_sta_info *si;
	struct rtw_rx_pkt_stat *pkt_stat = iter_data->pkt_stat;
	u8 *bssid = iter_data->bssid;

	if (!ether_addr_equal(vif->bss_conf.bssid, bssid))
		return;

	if (!(ether_addr_equal(vif->addr, hdr->addr1) ||
	      ieee80211_is_beacon(hdr->frame_control)))
		return;

	rtw_rx_phy_stat(rtwdev, pkt_stat, hdr);
	sta = ieee80211_find_sta_by_ifaddr(rtwdev->hw, hdr->addr2,
					   vif->addr);
	if (!sta)
		return;

	si = (struct rtw_sta_info *)sta->drv_priv;
	ewma_rssi_add(&si->avg_rssi, pkt_stat->rssi);
}

static void rtw_rx_addr_match(struct rtw_dev *rtwdev,
			      struct rtw_rx_pkt_stat *pkt_stat,
			      struct ieee80211_hdr *hdr)
{
	struct rtw_rx_addr_match_data data = {};

	if (pkt_stat->crc_err || pkt_stat->icv_err || !pkt_stat->phy_status ||
	    ieee80211_is_ctl(hdr->frame_control))
		return;

	data.rtwdev = rtwdev;
	data.hdr = hdr;
	data.pkt_stat = pkt_stat;
	data.bssid = get_hdr_bssid(hdr);

	rtw_iterate_vifs_atomic(rtwdev, rtw_rx_addr_match_iter, &data);
}

static void rtw_set_rx_freq_by_pktstat(struct rtw_rx_pkt_stat *pkt_stat,
				       struct ieee80211_rx_status *rx_status)
{
	rx_status->freq = pkt_stat->freq;
	rx_status->band = pkt_stat->band;
}

void rtw_update_rx_freq_from_ie(struct rtw_dev *rtwdev, struct sk_buff *skb,
				struct ieee80211_rx_status *rx_status,
				struct rtw_rx_pkt_stat *pkt_stat)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
	int channel = rtwdev->hal.current_channel;
	size_t hdr_len, ielen;
	int channel_number;
	u8 *variable;

	if (!test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		goto fill_rx_status;

	if (ieee80211_is_beacon(mgmt->frame_control)) {
		variable = mgmt->u.beacon.variable;
		hdr_len = offsetof(struct ieee80211_mgmt,
				   u.beacon.variable);
	} else if (ieee80211_is_probe_resp(mgmt->frame_control)) {
		variable = mgmt->u.probe_resp.variable;
		hdr_len = offsetof(struct ieee80211_mgmt,
				   u.probe_resp.variable);
	} else {
		goto fill_rx_status;
	}

	if (skb->len > hdr_len)
		ielen = skb->len - hdr_len;
	else
		goto fill_rx_status;

	channel_number = cfg80211_get_ies_channel_number(variable, ielen,
							 NL80211_BAND_2GHZ);
	if (channel_number != -1)
		channel = channel_number;

fill_rx_status:
	rtw_set_rx_freq_band(pkt_stat, channel);
	rtw_set_rx_freq_by_pktstat(pkt_stat, rx_status);
}
EXPORT_SYMBOL(rtw_update_rx_freq_from_ie);

static void rtw_rx_fill_rx_status(struct rtw_dev *rtwdev,
				  struct rtw_rx_pkt_stat *pkt_stat,
				  struct ieee80211_hdr *hdr,
				  struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_hw *hw = rtwdev->hw;
	u8 path;

	memset(rx_status, 0, sizeof(*rx_status));
	rx_status->freq = hw->conf.chandef.chan->center_freq;
	rx_status->band = hw->conf.chandef.chan->band;
	if (rtw_fw_feature_check(&rtwdev->fw, FW_FEATURE_SCAN_OFFLOAD) &&
	    test_bit(RTW_FLAG_SCANNING, rtwdev->flags))
		rtw_set_rx_freq_by_pktstat(pkt_stat, rx_status);
	if (pkt_stat->crc_err || pkt_stat->icv_err)
		rx_status->flag |= RX_FLAG_FAILED_FCS_CRC;
	if (pkt_stat->decrypted)
		rx_status->flag |= RX_FLAG_DECRYPTED;

	if (pkt_stat->rate >= DESC_RATEVHT1SS_MCS0)
		rx_status->encoding = RX_ENC_VHT;
	else if (pkt_stat->rate >= DESC_RATEMCS0)
		rx_status->encoding = RX_ENC_HT;

	if (rx_status->band == NL80211_BAND_5GHZ &&
	    pkt_stat->rate >= DESC_RATE6M &&
	    pkt_stat->rate <= DESC_RATE54M) {
		rx_status->rate_idx = pkt_stat->rate - DESC_RATE6M;
	} else if (rx_status->band == NL80211_BAND_2GHZ &&
		   pkt_stat->rate >= DESC_RATE1M &&
		   pkt_stat->rate <= DESC_RATE54M) {
		rx_status->rate_idx = pkt_stat->rate - DESC_RATE1M;
	} else if (pkt_stat->rate >= DESC_RATEMCS0) {
		rtw_desc_to_mcsrate(pkt_stat->rate, &rx_status->rate_idx,
				    &rx_status->nss);
	}

	rx_status->flag |= RX_FLAG_MACTIME_START;
	rx_status->mactime = pkt_stat->tsf_low;

	if (pkt_stat->bw == RTW_CHANNEL_WIDTH_80)
		rx_status->bw = RATE_INFO_BW_80;
	else if (pkt_stat->bw == RTW_CHANNEL_WIDTH_40)
		rx_status->bw = RATE_INFO_BW_40;
	else
		rx_status->bw = RATE_INFO_BW_20;

	if (pkt_stat->phy_status) {
		rx_status->signal = pkt_stat->signal_power;
		for (path = 0; path < rtwdev->hal.rf_path_num; path++) {
			rx_status->chains |= BIT(path);
			rx_status->chain_signal[path] = pkt_stat->rx_power[path];
		}
	} else {
		rx_status->flag |= RX_FLAG_NO_SIGNAL_VAL;
	}

	rtw_rx_addr_match(rtwdev, pkt_stat, hdr);

	/*
	 * Rtl8723cs and rtl8723bs drivers check for size < 14 or size > 8192
	 * and simply drop the packet.
	 */
	if (pkt_stat->pkt_len == 0 &&
	    (rtwdev->chip->id == RTW_CHIP_TYPE_8703B || rtw_is_8723bs(rtwdev))) {
		rx_status->flag |= RX_FLAG_NO_PSDU;
		rtw_dbg(rtwdev, RTW_DBG_RX, "zero length packet");
	}
}

static bool rtw_rx_ccmp_hdr_valid(struct rtw_rx_pkt_stat *pkt_stat,
				  struct ieee80211_hdr *hdr)
{
	u32 hdr_len = ieee80211_hdrlen(hdr->frame_control);
	u8 *iv;

	if (!ieee80211_has_protected(hdr->frame_control))
		return true;

	if (pkt_stat->pkt_len < hdr_len + IEEE80211_CCMP_HDR_LEN + FCS_LEN)
		return false;

	iv = (u8 *)hdr + hdr_len;

	/* reserved octet is zero, key ID octet has ExtIV set */
	return iv[2] == 0 && (iv[3] & BIT(5));
}

/* A forward PN jump this large between two frames of one key/TID is not
 * plausible; frames with garbage after the 802.11 header produce random
 * 48-bit PNs.
 */
#define RTW_RX_PN_GUARD_JUMP	BIT_ULL(24)

static u64 rtw_rx_ccmp_pn(const u8 *iv)
{
	return (u64)iv[0] | (u64)iv[1] << 8 | (u64)iv[4] << 16 |
	       (u64)iv[5] << 24 | (u64)iv[6] << 32 | (u64)iv[7] << 40;
}

static void rtw_rx_report_suspect(struct rtw_dev *rtwdev, const char *why,
				  struct rtw_rx_pkt_stat *pkt_stat,
				  void *rx_desc8, struct ieee80211_hdr *hdr,
				  u64 max_pn)
{
	u32 hdr_len = ieee80211_hdrlen(hdr->frame_control);
	u32 n = ++rtwdev->rx_pn_guard.events;
	u8 tid = ieee80211_is_data_qos(hdr->frame_control) ?
		 ieee80211_get_tid(hdr) : 0;
	u64 pn = 0;

	if (pkt_stat->pkt_len >= hdr_len + IEEE80211_CCMP_HDR_LEN)
		pn = rtw_rx_ccmp_pn((u8 *)hdr + hdr_len);

	if (n > RTW_RX_PN_GUARD_DUMPS) {
		dev_warn_ratelimited(rtwdev->dev,
				     "suspect hw-decrypted frame #%u (%s): tid %u max pn 0x%012llx pn 0x%012llx%s\n",
				     n, why, tid, max_pn, pn,
				     pkt_stat->decrypted ? "" : ", left to mac80211");
		return;
	}

	rtw_warn(rtwdev,
		 "suspect hw-decrypted frame #%u (%s): seq %u tid %u max pn 0x%012llx pn 0x%012llx len %u rate 0x%x%s\n",
		 n, why, ieee80211_get_sn(hdr), tid, max_pn, pn,
		 pkt_stat->pkt_len, pkt_stat->rate,
		 pkt_stat->decrypted ? "" : ", left to mac80211");
	print_hex_dump(KERN_WARNING, "rtw88 rx desc: ", DUMP_PREFIX_OFFSET,
		       16, 1, rx_desc8, (u8 *)hdr - (u8 *)rx_desc8, false);
	print_hex_dump(KERN_WARNING, "rtw88 rx frame: ", DUMP_PREFIX_OFFSET,
		       16, 1, hdr, min_t(u32, pkt_stat->pkt_len, hdr_len + 128),
		       false);
}

/* Some 8822CE devices occasionally deliver a frame whose descriptor and
 * 802.11 header are valid but whose remaining bytes, including the
 * cleartext CCMP header, are not that frame's, while the descriptor still
 * claims a successful hardware decryption. mac80211 then stores the garbage
 * PN as the key's last PN and drops every later frame as a replay until
 * the next rekey.
 *
 * The transmitter numbers all frames of a key from one PN counter, shared
 * by every TID (the receiver keeps per-TID replay counters only because of
 * QoS reordering). So track the highest PN seen per transmitter and
 * unicast/group key for hardware-decrypted data frames, and report frames
 * whose PN is implausibly far beyond it. A genuine frame on any TID cannot
 * be, however long that TID was idle. A suspect PN never becomes the
 * reference, so later genuine frames are not flagged and later bad frames
 * are still caught. With sw_decrypt_suspect set, a suspect frame is passed
 * to mac80211 as not decrypted, so it is verified in software and dropped
 * on MIC failure without moving the replay counter.
 */
static void rtw_rx_pn_guard(struct rtw_dev *rtwdev,
			    struct rtw_rx_pkt_stat *pkt_stat,
			    void *rx_desc8, struct ieee80211_hdr *hdr)
{
	struct rtw_rx_pn_guard *guard = &rtwdev->rx_pn_guard;
	u32 gen = atomic_read(&rtwdev->rx_pn_guard_gen);
	u32 hdr_len;
	u64 pn, max;
	u8 grp;

	if (guard->gen != gen) {
		memset(guard->valid, 0, sizeof(guard->valid));
		guard->gen = gen;
	}

	if (!ieee80211_is_data(hdr->frame_control) ||
	    !ieee80211_has_protected(hdr->frame_control))
		return;

	hdr_len = ieee80211_hdrlen(hdr->frame_control);
	if (pkt_stat->pkt_len < hdr_len + IEEE80211_CCMP_HDR_LEN + FCS_LEN)
		return;

	grp = is_multicast_ether_addr(hdr->addr1);
	pn = rtw_rx_ccmp_pn((u8 *)hdr + hdr_len);

	if (!guard->valid[grp] ||
	    !ether_addr_equal(guard->ta[grp], hdr->addr2)) {
		guard->pn[grp] = pn;
		ether_addr_copy(guard->ta[grp], hdr->addr2);
		guard->valid[grp] = true;
		return;
	}

	max = guard->pn[grp];
	if (pn <= max)
		return;

	if (pn - max > RTW_RX_PN_GUARD_JUMP) {
		if (READ_ONCE(rtw_sw_decrypt_suspect))
			pkt_stat->decrypted = false;
		rtw_rx_report_suspect(rtwdev, "PN jump", pkt_stat, rx_desc8,
				      hdr, max);
		return;
	}

	guard->pn[grp] = pn;
}

int rtw_rx_query_rx_desc(struct rtw_dev *rtwdev, void *rx_desc8,
			 struct rtw_rx_pkt_stat *pkt_stat,
			 struct ieee80211_rx_status *rx_status)
{
	u32 desc_sz = rtwdev->chip->rx_pkt_desc_sz;
	struct rtw_rx_desc *rx_desc = rx_desc8;
	struct ieee80211_hdr *hdr;
	u32 enc_type, swdec;
	void *phy_status;

	memset(pkt_stat, 0, sizeof(*pkt_stat));

	pkt_stat->pkt_len = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_PKT_LEN);
	pkt_stat->crc_err = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_CRC32);
	pkt_stat->icv_err = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_ICV_ERR);
	pkt_stat->drv_info_sz = le32_get_bits(rx_desc->w0,
					      RTW_RX_DESC_W0_DRV_INFO_SIZE);
	enc_type = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_ENC_TYPE);
	pkt_stat->shift = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_SHIFT);
	pkt_stat->phy_status = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_PHYST);
	swdec = le32_get_bits(rx_desc->w0, RTW_RX_DESC_W0_SWDEC);
	pkt_stat->decrypted = !swdec && enc_type != RX_DESC_ENC_NONE &&
			      !pkt_stat->crc_err && !pkt_stat->icv_err;

	pkt_stat->cam_id = le32_get_bits(rx_desc->w1, RTW_RX_DESC_W1_MACID);

	pkt_stat->is_c2h = le32_get_bits(rx_desc->w2, RTW_RX_DESC_W2_C2H);
	pkt_stat->ppdu_cnt = le32_get_bits(rx_desc->w2, RTW_RX_DESC_W2_PPDU_CNT);

	pkt_stat->rate = le32_get_bits(rx_desc->w3, RTW_RX_DESC_W3_RX_RATE);

	pkt_stat->bw = le32_get_bits(rx_desc->w4, RTW_RX_DESC_W4_BW);

	pkt_stat->tsf_low = le32_get_bits(rx_desc->w5, RTW_RX_DESC_W5_TSFL);

	if (unlikely(pkt_stat->rate >= DESC_RATE_MAX))
		return -EINVAL;

	if (unlikely(pkt_stat->drv_info_sz &&
		     pkt_stat->drv_info_sz != PHY_STATUS_SIZE))
		return -EINVAL;

	if (unlikely(pkt_stat->phy_status && !pkt_stat->drv_info_sz))
		return -EINVAL;

	if (unlikely(pkt_stat->pkt_len > IEEE80211_MAX_MPDU_LEN_VHT_11454))
		return -EINVAL;

	/* drv_info_sz is in unit of 8-bytes */
	pkt_stat->drv_info_sz *= 8;

	/* c2h cmd pkt's rx/phy status is not interested */
	if (pkt_stat->is_c2h)
		return 0;

	if (unlikely(pkt_stat->pkt_len <= FCS_LEN))
		return -EINVAL;

	phy_status = rx_desc8 + desc_sz + pkt_stat->shift;
	hdr = phy_status + pkt_stat->drv_info_sz;
	pkt_stat->hdr = hdr;

	if (pkt_stat->phy_status)
		rtwdev->chip->ops->query_phy_status(rtwdev, phy_status, pkt_stat);

	if (pkt_stat->decrypted && enc_type == RX_DESC_ENC_AES &&
	    !rtw_rx_ccmp_hdr_valid(pkt_stat, hdr)) {
		rtw_dbg(rtwdev, RTW_DBG_RX,
			"AES frame without a usable CCMP header, drop the decryption flag\n");
		pkt_stat->decrypted = false;
		rtw_rx_report_suspect(rtwdev, "bad CCMP header", pkt_stat,
				      rx_desc8, hdr, 0);
	}

	if (pkt_stat->decrypted && enc_type == RX_DESC_ENC_AES)
		rtw_rx_pn_guard(rtwdev, pkt_stat, rx_desc8, hdr);

	rtw_rx_fill_rx_status(rtwdev, pkt_stat, hdr, rx_status);

	return 0;
}
EXPORT_SYMBOL(rtw_rx_query_rx_desc);
