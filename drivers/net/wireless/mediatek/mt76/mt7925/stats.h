/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2026 MediaTek Inc. */

#ifndef __MT7925_STATS_H
#define __MT7925_STATS_H

#include <linux/if_ether.h>
#include <linux/types.h>
#include "../mt76_connac_mcu.h"

#define MT7925_STA_STATS_AC_NUM 4
#define MT7925_STA_STATS_BAND_NUM 2
#define MT7925_STA_STATS_AGG_RANGE_NUM 16
#define MT7925_STA_STATS_TX_QUALITY_NUM 4
#define MT7925_TX_MODE_NUM      16
#define MT7925_TX_RATE_CCK_NUM  5
#define MT7925_TX_RATE_BW_NUM   6

/* HW rate code (16-bit) field extract, mirror Gen4m CONNAC3X_HW_TX_RATE_TO_*.
 * The auto-rate table stores one rate code per entry.
 */
#define MT7925_HW_RATE_TO_MCS(_x)	(((_x) & GENMASK(5, 0)) >> 0)
#define MT7925_HW_RATE_TO_MODE(_x)	(((_x) & GENMASK(9, 6)) >> 6)
#define MT7925_HW_RATE_TO_NSS(_x)	(((_x) & GENMASK(13, 10)) >> 10)
#define MT7925_HW_RATE_TO_STBC(_x)	(((_x) & BIT(14)) >> 14)
#define MT7925_HW_RATE_TO_DCM(_x)	(((_x) & BIT(4)) >> 4)
#define MT7925_HW_RATE_TO_106T(_x)	(((_x) & BIT(5)) >> 5)
#define MT7925_HW_RATE_UNMASK_DCM(_r)	((u8)(_r) & 0xef)
#define MT7925_HW_RATE_UNMASK_106T(_r)	((u8)(_r) & 0xdf)

/* Number of entries in the firmware auto-rate table (mirror Gen4m
 * AUTO_RATE_NUM). RA cycles through these and reports which one is in use
 * via wtbl->rate_idx.
 */
#define MT7925_AUTO_RATE_NUM 8

/* Firmware EVENT_STATISTICS sub-event tags. The WTBL rate request uses cmd tag
 * UNI_CMD_ID_STATISTICS_WTBL (10) but the firmware replies with event tag 12.
 */
#define MT7925_UNI_EVENT_STATISTICS_WTBL 12

/* CONNAC3X TX Vector BBP latch bit fields (mirror cmm_asic_connac3x.h).
 * These operate on the 3-DWORD tx_vector[band].txv[3] array.
 */
#define MT7925_TXV_GET_TX_RATE(_txv)	((_txv)[2] & 0x7f)
#define MT7925_TXV_GET_TX_LDPC(_txv)	(((_txv)[2] & (0x1 << 7)) >> 7)
#define MT7925_TXV_GET_TX_STBC(_txv)	(((_txv)[0] & (0x3 << 6)) >> 6)
#define MT7925_TXV_GET_TX_FRMODE(_txv)	(((_txv)[0] & (0x7 << 8)) >> 8)
#define MT7925_TXV_GET_TX_MODE(_txv)	(((_txv)[0] & (0xf << 12)) >> 12)
#define MT7925_TXV_GET_TX_NSTS(_txv)	(((_txv)[2] & (0xfU << 28)) >> 28)
#define MT7925_TXV_GET_TX_PWR(_txv)	(((_txv)[0] & (0xff << 16)) >> 16)
#define MT7925_TXV_GET_TX_SGI(_txv)	(((_txv)[1] & (0x3 << 26)) >> 26)
#define MT7925_TXV_GET_TX_SPE_IDX(_txv)	(((_txv)[0] & (0x1f << 0)) >> 0)
#define MT7925_TXV_GET_TX_DCM(_txv)	(((_txv)[2] & (0x1 << 4)) >> 4)
#define MT7925_TXV_GET_TX_106T(_txv)	(((_txv)[2] & (0x1 << 5)) >> 5)
#define MT7925_TXV_RATE_UNMASK_DCM(_r)	((u8)(_r) & 0xef)
#define MT7925_TXV_RATE_UNMASK_106T(_r)	((u8)(_r) & 0xdf)

extern const char * const mt7925_tx_mode_str[MT7925_TX_MODE_NUM];
extern const char * const mt7925_tx_rate_cck_str[MT7925_TX_RATE_CCK_NUM];
extern const char * const mt7925_tx_rate_bw_str[MT7925_TX_RATE_BW_NUM];

/* TX rate mode, mirror Gen4m TX_RATE_MODE_* (see nic_rate.h). Used to decode
 * both the TX Vector BBP latch and the firmware auto-rate table rate codes.
 */
enum mt7925_tx_rate_mode {
	MT7925_TX_RATE_MODE_CCK = 0,
	MT7925_TX_RATE_MODE_OFDM = 1,
	MT7925_TX_RATE_MODE_HTMIX = 2,
	MT7925_TX_RATE_MODE_HTGF = 3,
	MT7925_TX_RATE_MODE_VHT = 4,
	MT7925_TX_RATE_MODE_PLR = 5,
	MT7925_TX_RATE_MODE_HE_SU = 8,
	MT7925_TX_RATE_MODE_HE_ER = 9,
	MT7925_TX_RATE_MODE_HE_TRIG = 10,
	MT7925_TX_RATE_MODE_HE_MU = 11,
	MT7925_TX_RATE_MODE_EHT_ER = 13,
	MT7925_TX_RATE_MODE_EHT_TRIG = 14,
	MT7925_TX_RATE_MODE_EHT_MU = 15,
};

enum stats_query_mib_array_index {
	QUERY_MIB_CNT_RX_FCS_ERR = 0,
	QUERY_MIB_CNT_RX_FIFO_OVERFLOW,
	QUERY_MIB_CNT_RX_MPDU,
	QUERY_MIB_CNT_AMPDU_RX_COUNT,
	QUERY_MIB_CNT_PF_DROP,
	QUERY_MIB_CNT_LEN_MISMATCH,
	QUERY_MIB_CNT_CHANNEL_IDLE,
	QUERY_MIB_CNT_CCA_NAV_TX_TIME,
	QUERY_MIB_CNT_MDRDY,
	QUERY_MIB_CNT_RX_CCK_MDRDY_TIME,
	QUERY_MIB_CNT_RX_OFDM_LG_MIXED_MDRDY_TIME,
	QUERY_MIB_CNT_RX_OFDM_GREEN_MDRDY_TIME,
	QUERY_MIB_CNT_P_CCA_TIME,
	QUERY_MIB_CNT_S_CCA_TIME,
	QUERY_MIB_CNT_P_ED_TIME,
	QUERY_MIB_CNT_BCN_TX,
	QUERY_MIB_CNT_TX_BW_40MHZ,
	QUERY_MIB_CNT_TX_BW_80MHZ,
	QUERY_MIB_CNT_TX_BW_160MHZ,
	QUERY_MIB_CNT_BSS0_BA_MISS,
	QUERY_MIB_CNT_BSS0_RTS_TX_CNT,
	QUERY_MIB_CNT_BSS0_FRAME_RETRY,
	QUERY_MIB_CNT_BSS0_FRAME_RETRY_2,
	QUERY_MIB_CNT_BSS0_RTS_RETRY,
	QUERY_MIB_CNT_BSS0_ACK_FAIL,
	QUERY_MIB_CNT_AMPDU,
	QUERY_MIB_CNT_AMPDU_MPDU,
	QUERY_MIB_CNT_AMPDU_ACKED,
	QUERY_MIB_CNT_MAX_NUM,
};

enum stats_trx_agg_range {
	STAT_MIB_CNT_TRX_AGG_RANGE0_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE1_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE2_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE3_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE4_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE5_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE6_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE7_IDX,
	STAT_MIB_CNT_TRX_AGG_RANGE_MAX_NUM
};

enum {
	MT7925_BW_20 = 0,
	MT7925_BW_40,
	MT7925_BW_80,
	MT7925_BW_160,
};

struct mt7925_sta_stats_ac {
	__le32 tx_fail;
	__le32 tx_retry;
} __packed;

struct mt7925_sta_stats_tx_vector {
	__le32 txv[3];
} __packed;

struct mt7925_sta_stats_mib {
	__le32 rx_mpdu_cnt;
	__le32 fcs_error;
	__le32 rx_fifo_full;
	__le32 ampdu_tx_sf_cnt;
	__le32 ampdu_tx_ack_sf_cnt;
	__le32 tx_range_ampdu_cnt[MT7925_STA_STATS_AGG_RANGE_NUM];
} __packed;

/* Firmware EVENT_STA_STATISTICS body. */
struct mt7925_sta_stats {
	u8 version;
	u8 rsv[3];
	__le32 flags;

	u8 sta_idx;
	u8 bss_idx;
	u8 wtbl_idx;
	u8 rsv1;

	u8 mac_addr[ETH_ALEN];
	u8 per;
	u8 rcpi;

	__le32 phy_mode;
	__le16 link_speed; /* 0.5 Mbit/s units */
	u8 link_quality;
	u8 link_reserved;

	__le32 tx_count;
	__le32 tx_fail_count;
	__le32 tx_life_timeout_count;
	__le32 tx_done_air_time;
	__le32 transmit_count;
	__le32 transmit_fail_count;

	struct mt7925_sta_stats_ac ac[MT7925_STA_STATS_AC_NUM];

	u8 temperature;
	u8 skip_ar;
	u8 ar_table_idx;
	u8 rate_entry_idx;
	u8 rate_entry_idx_prev;
	u8 tx_sgi_detect_pass_cnt;
	u8 ave_per;
	u8 rsv2;

	__le32 agg_range_ctrl[2];
	u8 range_mode;
	u8 rsv3[3];
	__le32 agg_range_ctrl_ext[6];

	u8 ar_state_curr;
	u8 ar_state_prev;
	u8 ar_action_type;
	u8 highest_rate_cnt;
	u8 lowest_rate_cnt;
	u8 rsv4;
	__le16 train_up;
	__le16 train_down;
	u8 rsv5[2];
	__le32 rate1_tx_cnt;
	__le32 rate1_fail_cnt;

	struct mt7925_sta_stats_tx_vector tx_vector[MT7925_STA_STATS_BAND_NUM];
	struct mt7925_sta_stats_mib mib[MT7925_STA_STATS_BAND_NUM];

	u8 is_force_tx_stream;
	u8 is_force_se_off;
	__le16 ra_running_cnt;
	u8 ra_status;
	u8 max_ampdu_factor;
	u8 tx_quality[MT7925_STA_STATS_TX_QUALITY_NUM];
	u8 tx_rate_up_penalty;
	u8 low_traffic_mode;
	u8 low_traffic_count;
	u8 low_traffic_dashboard;
	u8 dynamic_sgi_state;
	u8 dynamic_sgi_score;
	u8 dynamic_bw_state;
	u8 dynamic_gband_256qam_state;
	u8 vht_non_sp_rate_state;
	u8 rsv6[3];
	u8 rsv7[2];
} __packed;

struct mt7925_sta_rec_lookup {
	int sta_rec_idx;
};

/* Firmware auto-rate table snapshot, mirror Gen4m struct UNI_EVENT_STAT_WTBL.
 *
 * The firmware RA maintains up to MT7925_AUTO_RATE_NUM candidate rates in
 * rate_code[]. rate_idx tells which entry is currently in use, so a dump can
 * point at it with "-->". fcap is the bandwidth capability and the ldpc/sgi
 * flags describe the per-mode coding/guard-interval used for those rates.
 */
struct mt7925_wtbl_rate {
	__le16 tag;
	__le16 len;
	u8 rx_rcpi0;
	u8 rx_rcpi1;
	u8 cbrn;
	u8 fcap;
	u8 spe_idx;
	u8 g2;			/* SGI enabled, non-HE, BW20 */
	u8 g4;			/* SGI enabled, non-HE, BW40 */
	u8 g8;			/* SGI enabled, non-HE, BW80 */
	u8 g16;			/* SGI enabled, non-HE, BW160 */
	u8 g2_he;		/* GI, HE, BW20 */
	u8 g4_he;		/* GI, HE, BW40 */
	u8 g8_he;		/* GI, HE, BW80 */
	u8 g16_he;		/* GI, HE, BW160 */
	u8 gi_eht;		/* GI, EHT */
	u8 ht_ldpc;
	u8 vht_ldpc;
	u8 he_ldpc;
	u8 eht_ldpc;
	u8 af;
	u8 rate_idx;		/* index into rate_code[] currently in use */
	__le16 rate_code[MT7925_AUTO_RATE_NUM];
	__le32 rsv[4];
} __packed;

int
mt7925_get_ap_sta_rec_idx(struct mt792x_dev *dev, u8 *sta_rec_idx);

void
mt7925_print_last_tx_rate(struct seq_file *s, const __le32 le_txv[3]);

u8
mt7925_wtbl_get_sgi(const struct mt7925_wtbl_rate *wr, u8 txmode, u8 fcap);

u8
mt7925_wtbl_get_ldpc(const struct mt7925_wtbl_rate *wr, u8 txmode);

char *mt7925_hw_rate_ofdm_str(u8 ofdm_idx);

int mt792x_mcu_get_wtbl_rate(struct mt792x_dev *dev, u16 wlan_idx,
			     struct mt7925_wtbl_rate *rate);

int mt792x_mcu_get_stat(struct mt792x_dev *dev,
			struct mt7925_sta_stats *stats);

int mt7925_mcu_get_mib_info(struct mt792x_dev *dev, u8 band_idx,
			    const u32 *counters, u8 counter_num,
			    u64 *values);

/* GET_STAT_INFO (UNI_CMD_ID_GET_STATISTICS = 0x23),
 * sub-command UNI_CMD_GET_STATISTICS_TAG_LINK_QUALITY = 1.
 *
 * @band_idx:            band/BSS index (0..3)
 * @rssi:                out, moving-average beacon RSSI (dBm), may be NULL
 * @tx_link_speed_kbps:  out, TX link speed in kbit/s, may be NULL
 *
 * Returns 0 on success, -ENOENT if the band's link quality is not ready.
 *
 * NOTE: firmware provides only RSSI and TX rate here. RX rate/bandwidth are
 * not in this event; read wcid->rate for the RX rate.
 */
int mt7925_mcu_get_link_quality(struct mt792x_dev *dev, u8 band_idx,
				s8 *rssi, u32 *tx_link_speed_kbps);

#endif
