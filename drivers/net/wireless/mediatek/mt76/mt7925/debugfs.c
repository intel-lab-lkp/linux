// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2023 MediaTek Inc. */

#include <linux/math64.h>

#include "mt7925.h"
#include "mcu.h"
#include "stats.h"

static int
mt7925_reg_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	u32 regval = val;

	mt792x_mutex_acquire(dev);
	mt7925_mcu_regval(dev, dev->mt76.debugfs_reg, &regval, true);
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_reg_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;
	u32 regval;
	int ret;

	mt792x_mutex_acquire(dev);
	ret = mt7925_mcu_regval(dev, dev->mt76.debugfs_reg, &regval, false);
	mt792x_mutex_release(dev);
	if (!ret)
		*val = regval;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_regval, mt7925_reg_get, mt7925_reg_set,
			 "0x%08llx\n");
static int
mt7925_fw_debug_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;

	mt792x_mutex_acquire(dev);

	dev->fw_debug = (u8)val;
	mt7925_mcu_fw_log_2_host(dev, dev->fw_debug);

	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_fw_debug_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->fw_debug;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug, mt7925_fw_debug_get,
			 mt7925_fw_debug_set, "%lld\n");

DEFINE_SHOW_ATTRIBUTE(mt792x_tx_stats);

static void
mt7925_seq_puts_array(struct seq_file *file, const char *str,
		      s8 val[][2], int len, u8 band_idx)
{
	int i;

	seq_printf(file, "%-22s:", str);
	for (i = 0; i < len; i++)
		if (val[i][band_idx] == 127)
			seq_printf(file, " %6s", "N.A");
		else
			seq_printf(file, " %6d", val[i][band_idx]);
	seq_puts(file, "\n");
}

#define mt7925_print_txpwr_entry(prefix, rate, idx)	\
({							\
	mt7925_seq_puts_array(s, #prefix " (tmac)",	\
			      txpwr->rate,		\
			      ARRAY_SIZE(txpwr->rate),	\
			      idx);			\
})

static inline void
mt7925_eht_txpwr(struct seq_file *s, struct mt7925_txpwr *txpwr, u8 band_idx)
{
	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7", "mcs8", "mcs9", "mcs10", "mcs11",
		   "mcs12", "mcs13", "mcs14", "mcs15");
	mt7925_print_txpwr_entry(EHT26, eht26, band_idx);
	mt7925_print_txpwr_entry(EHT52, eht52, band_idx);
	mt7925_print_txpwr_entry(EHT106, eht106, band_idx);
	mt7925_print_txpwr_entry(EHT242, eht242, band_idx);
	mt7925_print_txpwr_entry(EHT484, eht484, band_idx);

	mt7925_print_txpwr_entry(EHT996, eht996, band_idx);
	mt7925_print_txpwr_entry(EHT996x2, eht996x2, band_idx);
	mt7925_print_txpwr_entry(EHT996x4, eht996x4, band_idx);
	mt7925_print_txpwr_entry(EHT26_52, eht26_52, band_idx);
	mt7925_print_txpwr_entry(EHT26_106, eht26_106, band_idx);
	mt7925_print_txpwr_entry(EHT484_242, eht484_242, band_idx);
	mt7925_print_txpwr_entry(EHT996_484, eht996_484, band_idx);
	mt7925_print_txpwr_entry(EHT996_484_242, eht996_484_242, band_idx);
	mt7925_print_txpwr_entry(EHT996x2_484, eht996x2_484, band_idx);
	mt7925_print_txpwr_entry(EHT996x3, eht996x3, band_idx);
	mt7925_print_txpwr_entry(EHT996x3_484, eht996x3_484, band_idx);
}

static int
mt7925_txpwr(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	struct mt7925_txpwr *txpwr = NULL;
	u8 band_idx = dev->mphy.band_idx;
	int ret = 0;

	txpwr = devm_kmalloc(dev->mt76.dev, sizeof(*txpwr), GFP_KERNEL);

	if (!txpwr)
		return -ENOMEM;

	mt792x_mutex_acquire(dev);
	ret = mt7925_get_txpwr_info(dev, band_idx, txpwr);
	mt792x_mutex_release(dev);

	if (ret)
		goto out;

	seq_printf(s, "%-22s  %6s %6s %6s %6s\n",
		   " ", "1m", "2m", "5m", "11m");
	mt7925_print_txpwr_entry(CCK, cck, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "6m", "9m", "12m", "18m", "24m", "36m",
		   "48m", "54m");
	mt7925_print_txpwr_entry(OFDM, ofdm, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7");
	mt7925_print_txpwr_entry(HT20, ht20, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7", "mcs32");
	mt7925_print_txpwr_entry(HT40, ht40, band_idx);

	seq_printf(s, "%-22s  %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s\n",
		   " ", "mcs0", "mcs1", "mcs2", "mcs3", "mcs4", "mcs5",
		   "mcs6", "mcs7", "mcs8", "mcs9", "mcs10", "mcs11");
	mt7925_print_txpwr_entry(VHT20, vht20, band_idx);
	mt7925_print_txpwr_entry(VHT40, vht40, band_idx);

	mt7925_print_txpwr_entry(VHT80, vht80, band_idx);
	mt7925_print_txpwr_entry(VHT160, vht160, band_idx);

	mt7925_print_txpwr_entry(HE26, he26, band_idx);
	mt7925_print_txpwr_entry(HE52, he52, band_idx);
	mt7925_print_txpwr_entry(HE106, he106, band_idx);
	mt7925_print_txpwr_entry(HE242, he242, band_idx);
	mt7925_print_txpwr_entry(HE484, he484, band_idx);

	mt7925_print_txpwr_entry(HE996, he996, band_idx);
	mt7925_print_txpwr_entry(HE996x2, he996x2, band_idx);

	mt7925_eht_txpwr(s, txpwr, band_idx);

out:
	devm_kfree(dev->mt76.dev, txpwr);
	return ret;
}

static int
mt7925_pm_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct mt76_connac_pm *pm = &dev->pm;

	if (mt76_is_usb(&dev->mt76))
		return -EOPNOTSUPP;

	mutex_lock(&dev->mt76.mutex);

	if (val == pm->enable_user)
		goto out;

	if (!pm->enable_user) {
		pm->stats.last_wake_event = jiffies;
		pm->stats.last_doze_event = jiffies;
	}
	/* make sure the chip is awake here and ps_work is scheduled
	 * just at end of the this routine.
	 */
	pm->enable = false;
	mt76_connac_pm_wake(&dev->mphy, pm);

	pm->enable_user = val;
	mt7925_set_runtime_pm(dev);
	mt76_connac_power_save_sched(&dev->mphy, pm);
out:
	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static int
mt7925_pm_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->pm.enable_user;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_pm, mt7925_pm_get, mt7925_pm_set, "%lld\n");

static int
mt7925_deep_sleep_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct mt76_connac_pm *pm = &dev->pm;
	bool monitor = !!(dev->mphy.hw->conf.flags & IEEE80211_CONF_MONITOR);
	bool enable = !!val;

	if (mt76_is_usb(&dev->mt76))
		return -EOPNOTSUPP;

	mt792x_mutex_acquire(dev);
	if (pm->ds_enable_user == enable)
		goto out;

	pm->ds_enable_user = enable;
	pm->ds_enable = enable && !monitor;
	mt7925_mcu_set_deep_sleep(dev, pm->ds_enable);
out:
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_deep_sleep_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->pm.ds_enable_user;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_ds, mt7925_deep_sleep_get,
			 mt7925_deep_sleep_set, "%lld\n");

DEFINE_DEBUGFS_ATTRIBUTE(fops_pm_idle_timeout, mt792x_pm_idle_timeout_get,
			 mt792x_pm_idle_timeout_set, "%lld\n");

static int mt7925_chip_reset(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	int ret = 0;

	switch (val) {
	case 1:
		/* Reset wifisys directly. */
		mt792x_reset(&dev->mt76);
		break;
	default:
		/* Collect the core dump before reset wifisys. */
		mt792x_mutex_acquire(dev);
		ret = mt7925_mcu_chip_config(dev, "assert");
		mt792x_mutex_release(dev);
		break;
	}

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_reset, NULL, mt7925_chip_reset, "%lld\n");

static const u32 stats_query_mib_counters[] = {
	UNI_CMD_MIB_CNT_RX_FCS_ERR,
	UNI_CMD_MIB_CNT_RX_FIFO_OVERFLOW,
	UNI_CMD_MIB_CNT_RX_MPDU,
	UNI_CMD_MIB_CNT_AMPDU_RX_COUNT,
	UNI_CMD_MIB_CNT_PF_DROP,
	UNI_CMD_MIB_CNT_LEN_MISMATCH,
	UNI_CMD_MIB_CNT_CHANNEL_IDLE,
	UNI_CMD_MIB_CNT_CCA_NAV_TX_TIME,
	UNI_CMD_MIB_CNT_MDRDY,
	UNI_CMD_MIB_CNT_RX_CCK_MDRDY_TIME,
	UNI_CMD_MIB_CNT_RX_OFDM_LG_MIXED_MDRDY_TIME,
	UNI_CMD_MIB_CNT_RX_OFDM_GREEN_MDRDY_TIME,
	UNI_CMD_MIB_CNT_P_CCA_TIME,
	UNI_CMD_MIB_CNT_S_CCA_TIME,
	UNI_CMD_MIB_CNT_P_ED_TIME,
	UNI_CMD_MIB_CNT_BCN_TX,
	UNI_CMD_MIB_CNT_TX_BW_40MHZ,
	UNI_CMD_MIB_CNT_TX_BW_80MHZ,
	UNI_CMD_MIB_CNT_TX_BW_160MHZ,
	UNI_CMD_MIB_CNT_BSS0_BA_MISS,
	UNI_CMD_MIB_CNT_BSS0_RTS_TX_CNT,
	UNI_CMD_MIB_CNT_BSS0_FRAME_RETRY,
	UNI_CMD_MIB_CNT_BSS0_FRAME_RETRY_2,
	UNI_CMD_MIB_CNT_BSS0_RTS_RETRY,
	UNI_CMD_MIB_CNT_BSS0_ACK_FAIL,
	UNI_CMD_MIB_CNT_AMPDU,
	UNI_CMD_MIB_CNT_AMPDU_MPDU,
	UNI_CMD_MIB_CNT_AMPDU_ACKED,
};

/* Dump the whole firmware auto-rate table and mark the entry currently in use
 * with "-->"
 */
static void
mt7925_dump_auto_rate_table(struct seq_file *s,
			    const struct mt7925_wtbl_rate *wr)
{
	u8 fcap = wr->fcap;
	u8 rate_idx = wr->rate_idx;
	u8 cbrn = wr->cbrn;
	int i;

	seq_printf(s, "%-22s  Cur Rate Index = %u\n", " ", rate_idx);

	for (i = 0; i < MT7925_AUTO_RATE_NUM; i++) {
		u16 rate_code = le16_to_cpu(wr->rate_code[i]);
		u8 txmode = MT7925_HW_RATE_TO_MODE(rate_code);
		u8 rate = MT7925_HW_RATE_TO_MCS(rate_code);
		u8 nsts = MT7925_HW_RATE_TO_NSS(rate_code) + 1;
		u8 stbc = MT7925_HW_RATE_TO_STBC(rate_code);
		u8 dcm = MT7925_HW_RATE_TO_DCM(rate_code);
		u8 ersu106t = MT7925_HW_RATE_TO_106T(rate_code);
		u8 sgi = mt7925_wtbl_get_sgi(wr, txmode, fcap);
		u8 ldpc = mt7925_wtbl_get_ldpc(wr, txmode);
		const char *mode_str = "N/A";

		if (dcm)
			rate = MT7925_HW_RATE_UNMASK_DCM(rate);
		if (ersu106t)
			rate = MT7925_HW_RATE_UNMASK_106T(rate);

		if (txmode < ARRAY_SIZE(mt7925_tx_mode_str))
			mode_str = mt7925_tx_mode_str[txmode];

		seq_printf(s, "Rate index[%d]    %s", i,
			   rate_idx == i ? "--> " : "    ");

		/* rate/MCS field */
		if (txmode == MT7925_TX_RATE_MODE_CCK)
			seq_printf(s, "%s, ",
				   mt7925_tx_rate_cck_str[rate & 0x3]);
		else if (txmode == MT7925_TX_RATE_MODE_OFDM)
			seq_printf(s, "%s, ", mt7925_hw_rate_ofdm_str(rate));
		else if (txmode == MT7925_TX_RATE_MODE_HTMIX ||
			 txmode == MT7925_TX_RATE_MODE_HTGF)
			seq_printf(s, "MCS%d, ", rate);
		else
			seq_printf(s, "%s%d_MCS%d, ",
				   stbc ? "NSTS" : "NSS", nsts, rate);

		/* bandwidth: mirror the CCK/OFDM -> BW20 and cbrn handling */
		if (txmode == MT7925_TX_RATE_MODE_CCK ||
		    txmode == MT7925_TX_RATE_MODE_OFDM)
			seq_printf(s, "%s, ", mt7925_tx_rate_bw_str[0]);
		else if (i > cbrn)
			seq_printf(s, "%s, ",
				   fcap < 5 ?
				   (fcap > MT7925_BW_20 ?
				    mt7925_tx_rate_bw_str[fcap - 1] :
				    mt7925_tx_rate_bw_str[fcap]) :
				   mt7925_tx_rate_bw_str[5]);
		else
			seq_printf(s, "%s, ",
				   fcap < 5 ? mt7925_tx_rate_bw_str[fcap] :
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
				   sgi == 0 ? "SGI" :
				   (sgi == 1 ? "MGI" : "LGI"));

		/* mode, DCM, 106t, STBC, coding */
		seq_printf(s, "%s%s%s%s%s\n",
			   mode_str,
			   dcm ? ", DCM" : "", ersu106t ? ", 106t" : "",
			   stbc ? ", STBC, " : ", ",
			   (ldpc == 0 ||
			    txmode == MT7925_TX_RATE_MODE_CCK ||
			    txmode == MT7925_TX_RATE_MODE_OFDM) ?
			   "BCC" : "LDPC");
	}
}

static int mt792x_stats(struct seq_file *s, void *data)
{
	s8 rssi = 0;
	u8 band_idx = 0;
	bool wtbl_rate_valid = false;
	int ret = 0;
	int i = 0;
	u32 stats_value = 0;
	u32 tx_link_speed_kbps = 0;
	u64 tx_total = 0;
	u64 tx_fail = 0;
	u64 tx_per = 0;
	u32 tx_per_rem = 0;
	/* UNI_CMD_MIB_DATA */
	u64 mib_values[ARRAY_SIZE(stats_query_mib_counters)];
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	struct mt7925_wtbl_rate wtbl_rate;
	struct mt7925_sta_stats *stats = NULL;

	band_idx = dev->mphy.band_idx;

	 stats = kzalloc(sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	/* Get STA_STATS */
	mt792x_mutex_acquire(dev);
	ret = mt792x_mcu_get_stat(dev, stats);
	mt792x_mutex_release(dev);
	if (ret)
		goto out;

	/* Get MIB_STATS */
	mt792x_mutex_acquire(dev);
	ret = mt7925_mcu_get_mib_info(dev, band_idx, stats_query_mib_counters,
				      ARRAY_SIZE(stats_query_mib_counters), mib_values);
	mt792x_mutex_release(dev);
	if (ret)
		goto out;

	/* Get Link_Speed and RSSI */
	mt792x_mutex_acquire(dev);
	ret = mt7925_mcu_get_link_quality(dev, stats->bss_idx, &rssi, &tx_link_speed_kbps);
	mt792x_mutex_release(dev);
	if (ret)
		goto out;

	/* Get the firmware auto-rate table (best effort; do not abort the
	 * whole stats dump if the WTBL query is not available).
	 */
	mt792x_mutex_acquire(dev);
	if (!mt792x_mcu_get_wtbl_rate(dev, stats->wtbl_idx, &wtbl_rate))
		wtbl_rate_valid = true;
	mt792x_mutex_release(dev);

	/* Print STA_STATS info */
	seq_printf(s, "(STA) connected AP MAC Address = %pM\n", stats->mac_addr);
	seq_printf(s, "%-22s  BssIdx = [%u]\n", " ", stats->bss_idx);
	seq_printf(s, "%-22s  StaRecIdx = [%u]\n",
		   " ", stats->sta_idx);
	seq_printf(s, "%-22s  RSSI = %d\n", " ", rssi);
	seq_printf(s, "%-22s  Link_Speed = %u (kbit/s)\n", " ", tx_link_speed_kbps);

	seq_printf(s, "%-22s  temperature = %u\n",
		   " ", stats->temperature);

	stats_value = le32_to_cpu(stats->transmit_count) - le32_to_cpu(stats->transmit_fail_count);
	seq_printf(s, "%-22s  Tx success = %u\n",
		   " ", stats_value);

	stats_value = le32_to_cpu(stats->tx_fail_count) - le32_to_cpu(stats->tx_life_timeout_count);
	seq_printf(s, "%-22s  Tx fail to Rcv ACK after retry = %u\n",
		   " ", stats_value);

	seq_printf(s, "%-22s  Rx Mpdu = %u\n",
		   " ", le32_to_cpu(stats->mib[band_idx].rx_mpdu_cnt));

	seq_printf(s, "%-22s  Rx Fcs Error = %u (MPDU)\n",
		   " ", le32_to_cpu(stats->mib[band_idx].fcs_error));
	seq_printf(s, "%-22s  Rx FIFO full = %u (MPDU)\n",
		   " ", le32_to_cpu(stats->mib[band_idx].rx_fifo_full));

	for (i = 0; i < STAT_MIB_CNT_TRX_AGG_RANGE_MAX_NUM; i++)
		seq_printf(s, "%-22s  TRX_AGG_RANGE[%d] = %u (PPDU)\n",
			   " ", i, le32_to_cpu(stats->mib[band_idx].tx_range_ampdu_cnt[i]));

	seq_printf(s, "%-22s  MPDUs in AMPDUs transmitted = %u (MPDU)\n",
		   " ", le32_to_cpu(stats->mib[band_idx].ampdu_tx_sf_cnt));
	seq_printf(s, "%-22s  MPDUs in AMPDUs transmitted with ACK reply = %u (MPDU)\n",
		   " ", le32_to_cpu(stats->mib[band_idx].ampdu_tx_ack_sf_cnt));
	seq_printf(s, "%-22s  rate1_tx_cnt = %u\n",
		   " ", le32_to_cpu(stats->rate1_tx_cnt));
	seq_printf(s, "%-22s  rate1_fail_cnt = %u\n",
		   " ", le32_to_cpu(stats->rate1_fail_cnt));
	seq_printf(s, "%-22s  train_up = %u\n",
		   " ", le16_to_cpu(stats->train_up));
	seq_printf(s, "%-22s  train_down = %u\n",
		   " ", le16_to_cpu(stats->train_down));
	seq_printf(s, "%-22s  is_force_tx_stream = %u\n",
		   " ", stats->is_force_tx_stream);
	seq_printf(s, "%-22s  is_force_se_off = %u\n",
		   " ", stats->is_force_se_off);
	seq_printf(s, "%-22s  max_ampdu_factor = %u\n",
		   " ", stats->max_ampdu_factor);
	seq_printf(s, "%-22s  tx_rate_up_penalty = %u\n",
		   " ", stats->tx_rate_up_penalty);
	seq_printf(s, "%-22s  low_traffic_mode = %u\n",
		   " ", stats->low_traffic_mode);
	seq_printf(s, "%-22s  low_traffic_count = %u\n",
		   " ", stats->low_traffic_count);
	seq_printf(s, "%-22s  low_traffic_dashboard = %u\n",
		   " ", stats->low_traffic_dashboard);
	seq_printf(s, "%-22s  dynamic_sgi_state = %u\n",
		   " ", stats->dynamic_sgi_state);
	seq_printf(s, "%-22s  dynamic_sgi_score = %u\n",
		   " ", stats->dynamic_sgi_score);
	seq_printf(s, "%-22s  dynamic_bw_state = %u\n",
		   " ", stats->dynamic_bw_state);
	seq_printf(s, "%-22s  dynamic_gband_256qam_state = %u\n",
		   " ", stats->dynamic_gband_256qam_state);
	seq_printf(s, "%-22s  vht_non_sp_rate_state = %u\n",
		   " ", stats->vht_non_sp_rate_state);

	/* Decode the TX Vector BBP latch to show the current TX MCS rate */
	if (band_idx < MT7925_STA_STATS_BAND_NUM)
		mt7925_print_last_tx_rate(s, stats->tx_vector[band_idx].txv);

	/* Dump the whole firmware auto-rate table and mark the entry currently */
	if (wtbl_rate_valid)
		mt7925_dump_auto_rate_table(s, &wtbl_rate);

	seq_puts(s, "\nmib state:\n");
	/* ===Rx Related Counters=== */
	seq_puts(s, "=== Rx Related Counters ===\n");
	seq_printf(s, "%-22s  Rx with CRC = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_RX_FCS_ERR]);
	seq_printf(s, "%-22s  Rx drop due to out of resource = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_RX_FIFO_OVERFLOW]);
	seq_printf(s, "%-22s  Rx Mpdu = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_RX_MPDU]);
	seq_printf(s, "%-22s  Rx AMpdu = %llu (PPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_AMPDU_RX_COUNT]);
	seq_printf(s, "%-22s  Rx PF Drop = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_PF_DROP]);
	seq_printf(s, "%-22s  Rx Len Mismatch = %llu (PPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_LEN_MISMATCH]);

	/* ===Phy/Timing Related Counters=== */
	seq_puts(s, "\n=== Phy/Timing Related Counters ===\n");
	seq_printf(s, "%-22s  ChannelIdleCnt = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_CHANNEL_IDLE]);
	seq_printf(s, "%-22s  CCA_NAV_Tx_Time = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_CCA_NAV_TX_TIME]);
	seq_printf(s, "%-22s  Rx_MDRDY_CNT = %llu (PPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_MDRDY]);
	seq_printf(s, "%-22s  CCK_MDRDY = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_RX_CCK_MDRDY_TIME]);
	seq_printf(s, "%-22s  OFDM_MDRDY = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_RX_OFDM_LG_MIXED_MDRDY_TIME]);
	seq_printf(s, "%-22s  OFDM_GREEN_MDRDY = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_RX_OFDM_GREEN_MDRDY_TIME]);
	seq_printf(s, "%-22s  Prim CCA Time = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_P_CCA_TIME]);
	seq_printf(s, "%-22s  Sec CCA Time = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_S_CCA_TIME]);
	seq_printf(s, "%-22s  Prim ED Time = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_P_ED_TIME]);

	/* ===Tx Related Counters(Generic)=== */
	seq_puts(s, "\n=== Tx Related Counters(Generic) ===\n");
	seq_printf(s, "%-22s  BeaconTxCnt = %llu\n",
		   " ", mib_values[QUERY_MIB_CNT_BCN_TX]);
	seq_printf(s, "%-22s  Tx 40MHz Cnt = %llu  (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_TX_BW_40MHZ]);
	seq_printf(s, "%-22s  Tx 80MHz Cnt = %llu  (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_TX_BW_80MHZ]);
	seq_printf(s, "%-22s  Tx 160MHz Cnt = %llu  (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_TX_BW_160MHZ]);

	/* ===BSSID[0] Related Counters=== */
	seq_puts(s, "\n=== BSSID[0] Related Counters ===\n");
	seq_printf(s, "%-22s  BA Miss Cnt = %llu  (PPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_BSS0_BA_MISS]);
	seq_printf(s, "%-22s  RTS Tx Cnt = %llu  (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_BSS0_RTS_TX_CNT]);
	seq_printf(s, "%-22s  Frame Retry Cnt = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_BSS0_FRAME_RETRY]);
	seq_printf(s, "%-22s  Frame Retry 2 Cnt = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_BSS0_FRAME_RETRY_2]);
	seq_printf(s, "%-22s  RTS Retry Cnt = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_BSS0_RTS_RETRY]);
	seq_printf(s, "%-22s  Ack Failed Cnt = %llu (PPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_BSS0_ACK_FAIL]);

	/* ===AMPDU Related Counters=== */
	seq_puts(s, "\n=== AMPDU Related Counters ===\n");
	seq_printf(s, "%-22s  Tx AMPDU_Pkt_Cnt = %llu  (PPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_AMPDU]);
	seq_printf(s, "%-22s  Tx AMPDU_MPDU_Pkt_Cnt = %llu  (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_AMPDU_MPDU]);
	seq_printf(s, "%-22s  AMPDU Tx success = %llu (MPDU)\n",
		   " ", mib_values[QUERY_MIB_CNT_AMPDU_ACKED]);

	tx_total = mib_values[QUERY_MIB_CNT_AMPDU_MPDU];
	tx_fail =  mib_values[QUERY_MIB_CNT_AMPDU_MPDU] - mib_values[QUERY_MIB_CNT_AMPDU_ACKED];
	tx_per = tx_total == 0 ? 0 : div64_u64(1000 * tx_fail, tx_total);
	seq_printf(s, "%-22s  AMPDU Tx fail count   = %llu  (MPDU), PER=%llu.%1llu%%\n",
		   " ",
		   tx_fail,
		   div_u64_rem(tx_per, 10, &tx_per_rem), (u64)tx_per_rem);

out:
	kfree(stats);
	return ret;
}

int mt7925_init_debugfs(struct mt792x_dev *dev)
{
	struct dentry *dir;

	dir = mt76_register_debugfs_fops(&dev->mphy, &fops_regval);

	if (mt76_is_mmio(&dev->mt76))
		debugfs_create_devm_seqfile(dev->mt76.dev, "xmit-queues",
					    dir, mt792x_queues_read);
	else
		debugfs_create_devm_seqfile(dev->mt76.dev, "xmit-queues",
					    dir, mt76_queues_read);

	debugfs_create_devm_seqfile(dev->mt76.dev, "acq", dir,
				    mt792x_queues_acq);
	debugfs_create_devm_seqfile(dev->mt76.dev, "txpower_sku", dir,
				    mt7925_txpwr);
	debugfs_create_file("tx_stats", 0400, dir, dev, &mt792x_tx_stats_fops);
	debugfs_create_file("fw_debug", 0600, dir, dev, &fops_fw_debug);
	debugfs_create_file("runtime-pm", 0600, dir, dev, &fops_pm);
	debugfs_create_file("idle-timeout", 0600, dir, dev,
			    &fops_pm_idle_timeout);
	debugfs_create_file("chip_reset", 0600, dir, dev, &fops_reset);
	debugfs_create_devm_seqfile(dev->mt76.dev, "runtime_pm_stats", dir,
				    mt792x_pm_stats);
	debugfs_create_file("deep-sleep", 0600, dir, dev, &fops_ds);

	debugfs_create_devm_seqfile(dev->mt76.dev, "stats", dir,
				    mt792x_stats);

	return 0;
}
