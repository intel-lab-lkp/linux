// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2026 MediaTek Inc. */

#include <linux/firmware.h>
#include <linux/fs.h>
#include "mt7925.h"
#include "mcu.h"
#include "mac.h"
#include "stats.h"

#define MT7925_STA_STATS_VERSION 1
#define MT7925_STA_STATS_VALID 1U

#define MT7925_MIB_MAX_CNT			32
#define MT7925_MIB_CNT_MAX_NUM		512  /* firmware UNI_CMD_MIB_CNT_MAX_NUM upper bound */

#define MT7925_LQ_BAND_NUM	4			/* Link quality band */

const char * const mt7925_tx_mode_str[] = {
	"CCK", "OFDM", "MM", "GF", "VHT", "PLR",
	"PLRP", "ALR", "HE_SU", "HE_ER", "HE_TRIG", "HE_MU",
	"EHT_ER", "TOF", "EHT_TRIG", "EHT_MU"
};

const char * const mt7925_tx_rate_cck_str[] = {
	"1M", "2M", "5.5M", "11M", "N/A"
};

const char * const mt7925_tx_rate_bw_str[] = {
	"BW20", "BW40", "BW80", "BW160/BW8080", "BW320", "N/A"
};

struct mt7925_sta_stats_req {
	u8 rsv[4];
	__le16 tag;
	__le16 len;
	u8 sta_rec_idx;
	u8 rsv1;
	u8 lls_read_clear;
	u8 reset_counter;
} __packed;

struct mt7925_sta_stats_event {
	u8 rsv[4];
	__le16 tag;
	__le16 len;
	struct mt7925_sta_stats stats;
} __packed;

/* WTBL auto-rate request TLV, mirror Gen4m struct UNI_CMD_STAT_WTBL.
 * The RA tag reuses UNI_CMD_ID_STATISTICS_WTBL (10).
 */
struct mt7925_wtbl_rate_req {
	u8 rsv[4];
	__le16 tag;
	__le16 len;
	__le16 wlan_idx;
	__le16 rsv1;
} __packed;

/* WTBL auto-rate event TLV. Firmware answers cmd tag 10 with event tag 12. */
struct mt7925_wtbl_rate_event {
	u8 rsv[4];
	struct mt7925_wtbl_rate rate;
} __packed;

/* fixed field, mirror UNI_(CMD|EVENT)_MIB_INFO */
struct mt7925_mib_hdr {
	u8 band_idx;
	u8 rsv[3];
} __packed;

/* One requested/returned MIB counter, mirror UNI_(CMD|EVENT)_MIB_DATA_T. */
struct mt7925_mib_data {
	__le16 tag;	/* UNI_(CMD|EVENT)_MIB_DATA = 0 */
	__le16 len;
	__le32 counter;	/* MIB counter ID (enum mt7925_mib_counter) */
	__le64 data;	/* only valid on event; ignored on cmd */
} __packed;

/* per-band link quality entry, mirror UNI_LINK_QUALITY */
struct mt7925_link_quality {
	s8 rssi;		/* moving-average beacon RSSI of connected AP */
	s8 link_quality;	/* always 0 */
	__le16 link_speed;	/* TX rate1, unit 500 Kb/s */
	u8 medium_busy_pct;	/* always 0 */
	u8 is_ready;		/* 0: fields invalid, 1: valid */
	u8 rsv[2];
} __packed;

/* request TLV, mirror UNI_CMD_LINK_QUALITY (header + tag/len only) */
struct mt7925_lq_req {
	u8 rsv[4];		/* UNI_CMD_GET_STATISTICS fixed field */
	__le16 tag;		/* UNI_CMD_GET_STATISTICS_TAG_LINK_QUALITY = 1 */
	__le16 len;
} __packed;

/* event TLV, mirror UNI_EVENT_LINK_QUALITY */
struct mt7925_lq_event {
	u8 rsv[4];		/* UNI_EVENT_STATISTICS fixed field */
	__le16 tag;		/* UNI_EVENT_ID_STATISTICS_LINK_QUALITY = 1 */
	__le16 len;
	struct mt7925_link_quality lq[MT7925_LQ_BAND_NUM];
} __packed;

static void
mt7925_get_ap_sta_rec_idx_iter(void *data, u8 *mac,
			       struct ieee80211_vif *vif)
{
	struct mt7925_sta_rec_lookup *lookup = data;
	struct mt792x_vif *mvif;
	struct mt792x_sta *msta;

	if (lookup->sta_rec_idx >= 0)
		return;

	if (vif->type != NL80211_IFTYPE_STATION || !vif->cfg.assoc)
		return;

	mvif = (struct mt792x_vif *)vif->drv_priv;
	msta = mvif->wep_sta;
	if (!msta)
		return;

	if (!msta->deflink.wcid.sta)
		return;

	lookup->sta_rec_idx = msta->deflink.wcid.idx;
}

int
mt7925_get_ap_sta_rec_idx(struct mt792x_dev *dev, u8 *sta_rec_idx)
{
	struct mt7925_sta_rec_lookup lookup = {
		.sta_rec_idx = -1,
	};

	lockdep_assert_held(&dev->mt76.mutex);

	ieee80211_iterate_active_interfaces(dev->mphy.hw,
					    IEEE80211_IFACE_ITER_RESUME_ALL,
					    mt7925_get_ap_sta_rec_idx_iter,
					    &lookup);

	if (lookup.sta_rec_idx < 0)
		return -ENOTCONN;

	if (lookup.sta_rec_idx > U8_MAX)
		return -ERANGE;

	*sta_rec_idx = lookup.sta_rec_idx;

	return 0;
}

/* Decode the TX Vector BBP latch and print the last TX rate */
void
mt7925_print_last_tx_rate(struct seq_file *s, const __le32 le_txv[3])
{
	const char *mode_str = "N/A";
	u8 rate, txmode, frmode, sgi, ldpc, nsts, stbc, spe, dcm, ersu106t;
	s8 txpwr, pos_txpwr;
	u32 txv[3];

	txv[0] = le32_to_cpu(le_txv[0]);
	txv[1] = le32_to_cpu(le_txv[1]);
	txv[2] = le32_to_cpu(le_txv[2]);

	if (txv[0] == 0xffffffff) {
		seq_printf(s, "%-22s  %s = %s\n", " ", "Last TX Rate", "N/A");
		seq_printf(s, "%-22s  %s = %s\n",
			   " ", "Chip Out TX Power", "N/A");
		return;
	}

	rate = MT7925_TXV_GET_TX_RATE(txv);
	txmode = MT7925_TXV_GET_TX_MODE(txv);
	frmode = MT7925_TXV_GET_TX_FRMODE(txv);
	nsts = MT7925_TXV_GET_TX_NSTS(txv) + 1;
	sgi = MT7925_TXV_GET_TX_SGI(txv);
	ldpc = MT7925_TXV_GET_TX_LDPC(txv);
	stbc = MT7925_TXV_GET_TX_STBC(txv);
	txpwr = MT7925_TXV_GET_TX_PWR(txv);
	spe = MT7925_TXV_GET_TX_SPE_IDX(txv);
	dcm = MT7925_TXV_GET_TX_DCM(txv);
	ersu106t = MT7925_TXV_GET_TX_106T(txv);

	if (dcm)
		rate = MT7925_TXV_RATE_UNMASK_DCM(rate);
	if (ersu106t)
		rate = MT7925_TXV_RATE_UNMASK_106T(rate);

	if (txmode < ARRAY_SIZE(mt7925_tx_mode_str))
		mode_str = mt7925_tx_mode_str[txmode];

	seq_printf(s, "%-22s  %s = ", " ", "Last TX Rate");

	/* rate/MCS field */
	if (txmode == MT7925_TX_RATE_MODE_CCK)
		seq_printf(s, "%s, ",
			   rate < 4 ? mt7925_tx_rate_cck_str[rate] :
			   (((rate >= 5) && (rate <= 7)) ?
			    mt7925_tx_rate_cck_str[rate - 4] :
			    mt7925_tx_rate_cck_str[4]));
	else if (txmode == MT7925_TX_RATE_MODE_OFDM)
		seq_printf(s, "%s, ", mt7925_hw_rate_ofdm_str(rate));
	else if (txmode == MT7925_TX_RATE_MODE_HTMIX ||
		 txmode == MT7925_TX_RATE_MODE_HTGF)
		seq_printf(s, "MCS%d, ", rate);
	else
		seq_printf(s, "%s%d_MCS%d, ",
			   stbc ? "NSTS" : "NSS", nsts, rate);

	/* bandwidth */
	seq_printf(s, "%s, ",
		   frmode < 5 ? mt7925_tx_rate_bw_str[frmode] :
		   mt7925_tx_rate_bw_str[5]);

	/* GI / preamble */
	if (txmode == MT7925_TX_RATE_MODE_CCK)
		seq_printf(s, "%s, ", rate < 4 ? "LP" : "SP");
	else if (txmode == MT7925_TX_RATE_MODE_OFDM)
		; /* OFDM has no GI/preamble to print */
	else if (txmode == MT7925_TX_RATE_MODE_HTMIX ||
		 txmode == MT7925_TX_RATE_MODE_HTGF ||
		 txmode == MT7925_TX_RATE_MODE_VHT ||
		 txmode == MT7925_TX_RATE_MODE_PLR)
		seq_printf(s, "%s, ", sgi == 0 ? "LGI" : "SGI");
	else
		seq_printf(s, "%s, ",
			   sgi == 0 ? "SGI" : (sgi == 1 ? "MGI" : "LGI"));

	/* mode, DCM, 106t, STBC, coding, SPE */
	seq_printf(s, "%s%s%s%s%s%s%d\n",
		   mode_str,
		   dcm ? ", DCM" : "", ersu106t ? ", 106t" : "",
		   stbc ? ", STBC, " : ", ", ldpc == 0 ? "BCC" : "LDPC",
		   ", SPE", spe);

	/* chip out TX power (unit: 0.5 dBm) */
	pos_txpwr = (txpwr < 0) ? (~txpwr + 1) : txpwr;
	seq_printf(s, "%-22s  %s = %c%d.%1d dBm\n",
		   " ", "Chip Out TX Power",
		   (txpwr < 0) ? '-' : '+',
		   (pos_txpwr / 2), 5 * (pos_txpwr % 2));
}

/* Guard interval used for the auto-rate entries, mirror Gen4m
 * connac3x_get_sgi_info_from_fw(): pick the per-mode/per-BW flag.
 */
u8
mt7925_wtbl_get_sgi(const struct mt7925_wtbl_rate *wr, u8 txmode, u8 fcap)
{
	bool he = (txmode == MT7925_TX_RATE_MODE_HE_SU ||
		   txmode == MT7925_TX_RATE_MODE_HE_ER ||
		   txmode == MT7925_TX_RATE_MODE_HE_TRIG ||
		   txmode == MT7925_TX_RATE_MODE_HE_MU);
	bool eht = (txmode == MT7925_TX_RATE_MODE_EHT_ER ||
		    txmode == MT7925_TX_RATE_MODE_EHT_TRIG ||
		    txmode == MT7925_TX_RATE_MODE_EHT_MU);

	if (eht)
		return wr->gi_eht;

	switch (fcap) {
	case MT7925_BW_20:
		return he ? wr->g2_he : wr->g2;
	case MT7925_BW_40:
		return he ? wr->g4_he : wr->g4;
	case MT7925_BW_80:
		return he ? wr->g8_he : wr->g8;
	case MT7925_BW_160:
		return he ? wr->g16_he : wr->g16;
	default:
		return 0;
	}
}

/* Coding used for the auto-rate entries, mirror Gen4m
 * connac3x_wtbl_get_ldpc_info_from_fw(): 0 = BCC, 1 = LDPC.
 */
u8
mt7925_wtbl_get_ldpc(const struct mt7925_wtbl_rate *wr, u8 txmode)
{
	switch (txmode) {
	case MT7925_TX_RATE_MODE_HTMIX:
	case MT7925_TX_RATE_MODE_HTGF:
		return wr->ht_ldpc;
	case MT7925_TX_RATE_MODE_VHT:
		return wr->vht_ldpc;
	case MT7925_TX_RATE_MODE_HE_SU:
	case MT7925_TX_RATE_MODE_HE_ER:
	case MT7925_TX_RATE_MODE_HE_TRIG:
	case MT7925_TX_RATE_MODE_HE_MU:
		return wr->he_ldpc;
	case MT7925_TX_RATE_MODE_EHT_ER:
	case MT7925_TX_RATE_MODE_EHT_TRIG:
	case MT7925_TX_RATE_MODE_EHT_MU:
		return wr->eht_ldpc;
	default:
		return 0;
	}
}

int mt792x_mcu_get_stat(struct mt792x_dev *dev,
			struct mt7925_sta_stats *stats)
{
	u16 len = 0;
	int ret = 0;
	struct mt7925_sta_stats_event *event = NULL;
	struct sk_buff *skb = NULL;
	struct mt7925_sta_stats_req req = {
		.tag = cpu_to_le16(UNI_CMD_ID_STATISTICS_STA),
		.len = cpu_to_le16(sizeof(req) - sizeof(req.rsv)),
		.lls_read_clear = false,
		.reset_counter = false,
	};

	ret = mt7925_get_ap_sta_rec_idx(dev, &req.sta_rec_idx);
	if (ret)
		return ret;

	if (!stats)
		return -EINVAL;

	ret = mt76_mcu_send_and_get_msg(&dev->mt76,
					MCU_UNI_QUERY(GET_STAT_INFO),
					&req, sizeof(req), true, &skb);

	if (ret)
		return ret;

	event = (struct mt7925_sta_stats_event *)skb->data;

	if (le16_to_cpu(event->tag) != UNI_CMD_ID_STATISTICS_STA) {
		ret = -EPROTO;
		goto out;
	}

	len = le16_to_cpu(event->len);
	if (len < sizeof(*event) - sizeof(event->rsv) ||
	    len > skb->len - sizeof(event->rsv) || !IS_ALIGNED(len, 4)) {
		ret = -EMSGSIZE;
		goto out;
	}

	if (event->stats.version != MT7925_STA_STATS_VERSION) {
		ret = -EPROTO;
		goto out;
	}

	if (!(le32_to_cpu(event->stats.flags) & MT7925_STA_STATS_VALID)) {
		ret = -ENOENT;
		goto out;
	}

	memcpy(stats, &event->stats, sizeof(*stats));

out:
	dev_kfree_skb(skb);
	return ret;
}

int mt7925_mcu_get_mib_info(struct mt792x_dev *dev, u8 band_idx,
			    const u32 *counters, u8 counter_num,
			    u64 *values)
{
	u8 *req;
	u8 *tlv, *tlv_end;
	u16 req_len;
	int i, ret;
	struct mt7925_mib_hdr *req_hdr;
	struct mt7925_mib_data *req_tlv;
	struct sk_buff *skb;

	if (!counters || !values || !counter_num ||
	    counter_num > MT7925_MIB_MAX_CNT)
		return -EINVAL;

	/* clear output; counters not returned by firmware stay 0 */
	memset(values, 0, counter_num * sizeof(*values));

	req_len = sizeof(struct mt7925_mib_hdr) +
		  counter_num * sizeof(struct mt7925_mib_data);

	req = kzalloc(req_len, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req_hdr = (struct mt7925_mib_hdr *)req;
	req_hdr->band_idx = band_idx;

	req_tlv = (struct mt7925_mib_data *)(req + sizeof(*req_hdr));
	for (i = 0; i < counter_num; i++) {
		req_tlv[i].tag = cpu_to_le16(UNI_CMD_MIB_DATA);
		req_tlv[i].len = cpu_to_le16(sizeof(struct mt7925_mib_data));
		req_tlv[i].counter = cpu_to_le32(counters[i]);
	}

	ret = mt76_mcu_send_and_get_msg(&dev->mt76,
					MCU_UNI_QUERY(GET_MIB_INFO), req,
					req_len, true, &skb);
	kfree(req);
	if (ret)
		return ret;

	if (skb->len < sizeof(struct mt7925_mib_hdr)) {
		ret = -EMSGSIZE;
		goto out;
	}

	/* walk the returned TLVs, match each counter ID to the request */
	tlv = skb->data + sizeof(struct mt7925_mib_hdr);
	tlv_end = skb->data + skb->len;

	while (tlv + sizeof(struct mt7925_mib_data) <= tlv_end) {
		struct mt7925_mib_data *evt_tlv =
			(struct mt7925_mib_data *)tlv;
		u16 tag = le16_to_cpu(evt_tlv->tag);
		u16 len = le16_to_cpu(evt_tlv->len);
		u32 counter;

		if (len < sizeof(struct mt7925_mib_data) ||
		    tlv + len > tlv_end)
			break;

		if (tag == UNI_CMD_MIB_DATA) {
			counter = le32_to_cpu(evt_tlv->counter);

			if (counter < MT7925_MIB_CNT_MAX_NUM) {
				for (i = 0; i < counter_num; i++) {
					if (counters[i] == counter) {
						values[i] =
						  le64_to_cpu(evt_tlv->data);
						break;
					}
				}
			}
		}

		tlv += len;
	}

	ret = 0;

out:
	dev_kfree_skb(skb);

	return ret;
}

char *mt7925_hw_rate_ofdm_str(u8 ofdm_idx)
{
	char * const ofdm_str[] = {
		"6M", "9M", "12M", "18M", "24M", "36M", "48M", "54M", "N/A"
	};

	switch (ofdm_idx) {
	case 11: return ofdm_str[0];	/* 6M */
	case 15: return ofdm_str[1];	/* 9M */
	case 10: return ofdm_str[2];	/* 12M */
	case 14: return ofdm_str[3];	/* 18M */
	case 9:  return ofdm_str[4];	/* 24M */
	case 13: return ofdm_str[5];	/* 36M */
	case 8:  return ofdm_str[6];	/* 48M */
	case 12: return ofdm_str[7];	/* 54M */
	default: return ofdm_str[8];
	}
}

/* mt792x_mcu_get_wtbl_rate - fetch the firmware auto-rate table for a WCID
 * @dev:	device
 * @wlan_idx:	HW WTBL / WCID index (see mt7925_sta_stats.wtbl_idx)
 * @rate:	out, filled with the auto-rate table snapshot
 *
 * Mirror of Gen4m wlanoidGetStatWtbl(): send UNI_CMD_GET_STAT_INFO with the
 * WTBL rate TLV (cmd tag 10) and parse the reply (event tag 12) into @rate.
 *
 * Return: 0 on success, negative errno otherwise.
 */
int mt792x_mcu_get_wtbl_rate(struct mt792x_dev *dev, u16 wlan_idx,
			     struct mt7925_wtbl_rate *rate)
{
	int ret;
	struct mt7925_wtbl_rate_event *event = NULL;
	struct sk_buff *skb = NULL;
	struct mt7925_wtbl_rate_req req = {
		.tag = cpu_to_le16(UNI_CMD_ID_STATISTICS_WTBL),
		.len = cpu_to_le16(sizeof(req) - sizeof(req.rsv)),
		.wlan_idx = cpu_to_le16(wlan_idx),
	};

	if (!rate)
		return -EINVAL;

	ret = mt76_mcu_send_and_get_msg(&dev->mt76,
					MCU_UNI_QUERY(GET_STAT_INFO),
					&req, sizeof(req), true, &skb);
	if (ret)
		return ret;

	if (skb->len < sizeof(*event)) {
		ret = -EMSGSIZE;
		goto out;
	}

	event = (struct mt7925_wtbl_rate_event *)skb->data;

	if (le16_to_cpu(event->rate.tag) != MT7925_UNI_EVENT_STATISTICS_WTBL) {
		ret = -EPROTO;
		goto out;
	}

	memcpy(rate, &event->rate, sizeof(*rate));
	ret = 0;

out:
	dev_kfree_skb(skb);
	return ret;
}

/**
 * mt7925_mcu_get_link_quality - query RSSI and TX link speed for a band
 * @dev:	device
 * @band_idx:	band/BSS index (0..3)
 * @rssi:	out, moving-average beacon RSSI in dBm (may be NULL)
 * @tx_link_speed_kbps: out, TX rate in kbit/s (may be NULL)
 *
 * Return: 0 on success, -ENOENT if the band's link quality is not ready.
 *
 * Only RSSI and TX rate come from firmware here; for RX rate read wcid->rate.
 */
int mt7925_mcu_get_link_quality(struct mt792x_dev *dev, u8 band_idx,
				s8 *rssi, u32 *tx_link_speed_kbps)
{
	int ret;
	struct mt7925_lq_event *event;
	struct mt7925_link_quality *lq;
	struct sk_buff *skb;
	struct mt7925_lq_req req = {
		.tag = cpu_to_le16(UNI_CMD_ID_STATISTICS_LINK_QUALITY),
		.len = cpu_to_le16(sizeof(req) - sizeof(req.rsv)),
	};

	if (band_idx >= MT7925_LQ_BAND_NUM)
		return -EINVAL;

	ret = mt76_mcu_send_and_get_msg(&dev->mt76,
					MCU_UNI_QUERY(GET_STAT_INFO), &req,
					sizeof(req), true, &skb);
	if (ret)
		return ret;

	if (skb->len < sizeof(*event)) {
		ret = -EMSGSIZE;
		goto out;
	}

	event = (struct mt7925_lq_event *)skb->data;

	if (le16_to_cpu(event->tag) != UNI_CMD_ID_STATISTICS_LINK_QUALITY) {
		ret = -EPROTO;
		goto out;
	}

	lq = &event->lq[band_idx];
	if (!lq->is_ready) {
		ret = -ENOENT;
		goto out;
	}

	if (rssi)
		*rssi = lq->rssi;

	/* firmware reports TX rate in 500 Kb/s units -> kbit/s */
	if (tx_link_speed_kbps)
		*tx_link_speed_kbps = le16_to_cpu(lq->link_speed) * 500;

	ret = 0;

out:
	dev_kfree_skb(skb);

	return ret;
}
