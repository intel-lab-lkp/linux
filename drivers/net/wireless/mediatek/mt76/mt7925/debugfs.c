// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2023 MediaTek Inc. */

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

/* fixedrate input format (12 hex digits):
 * 0x[WCID(2)][Mode][BW][MCS][Nss][SGI][Preamble][STBC][LDPC][SPE_EN][HeLtf]
 * The RA tag is fixed to SET_FIXED_RATE internally.
 * WCID is 2 nibbles, all others are 1 nibble.
 *
 * [WCID]    Wireless Client ID
 * [Mode]    CCK=0, OFDM=1, HT=2, GF=3, VHT=4, PLR=5, HE_SU=8, HE_ER_SU=9,
 *           HE_TRIG=10, HE_MU=11, EHT_ER=13, EHT_TB=14, EHT_SU and EHT_MU=15
 * [BW]      BW20=0, BW40=1, BW80=2, BW160=3, BW320=4
 * [MCS]     CCK=0~3, OFDM=0~7, HT=0~32, VHT=0~9, HE=0~11, EHT=0~13, EHT_ER=14~15
 * [Nss]     1~8
 * [GI]      HT/VHT: 0=Long, 1=Short; HE/EHT: 0=0.8us, 1=1.6us, 2=3.2us
 * [Preamble] Long=0, Short=other
 * [STBC]    Enable=1, Disable=0
 * [LDPC]    BCC=0, LDPC=1 (driver sends the FW's "on" value 7)
 * [SPE_EN]  spatial extension index
 * [HeLtf]   1X=0, 2X=1, 4X=2 (NOTE: EHT modes require HeLtf=1)
 */
static int
mt7925_fixedrate_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	struct mt7925_ra_fixed_rate_v1 rate = {};

	dev->fixed_rate = val;

	rate.wlan_idx       = cpu_to_le16((val >> 40) & 0xff);
	rate.phy_mode       = (val >> 36) & 0xf;
	rate.bw             = (val >> 32) & 0xf;
	rate.mcs            = (val >> 28) & 0xf;
	rate.nss            = (val >> 24) & 0xf;
	rate.short_gi       = cpu_to_le16((val >> 20) & 0xf);
	rate.short_preamble = (val >> 16) & 0xf;
	rate.stbc           = (val >> 12) & 0xf;
	rate.ecc            = ((val >> 8) & 0xf) ? 7 : 0;
	rate.spe            = (val >> 4) & 0xf;
	rate.he_ltf         = cpu_to_le16((val >> 0) & 0xf);

	mt792x_mutex_acquire(dev);
	mt7925_mcu_set_fixed_rate(dev, &rate);
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_fixedrate_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->fixed_rate;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_fixedrate, mt7925_fixedrate_get,
			 mt7925_fixedrate_set, "0x%llx\n");

/* autorate input format (6 hex digits):
 * 0x[Tag(2)][WCID(2)][En][Mode]
 * The RA tag is fixed to SET_AUTO_RATE internally.
 * WCID is 2 nibbles, En and Mode are 1 nibble each.
 *
 * [WCID] Wireless Client ID
 * [En]   Auto rate: Enable=1, Disable=0
 * [Mode] don't care (RA picks the rate)
 */
static int
mt7925_autorate_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;
	u16 wlan_idx = (val >> 8) & 0xff;
	bool enable = (val >> 4) & 0xf;
	u8 mode = val & 0xf;

	dev->auto_rate = val;

	mt792x_mutex_acquire(dev);
	mt7925_mcu_set_auto_rate(dev, wlan_idx, enable, mode);
	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_autorate_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->auto_rate;

	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(fops_autorate, mt7925_autorate_get,
			 mt7925_autorate_set, "0x%llx\n");

/* WTBL dump
 *
 * Usage:
 *   echo <idx> > /sys/kernel/debug/ieee80211/phy1/mt76/wtbl_idx
 *   cat  /sys/kernel/debug/ieee80211/phy1/mt76/get_wtbl
 *
 * LMAC WTBL has 36 dwords per entry (connac3x).
 */
#define MT792x_WTBL_LMAC_DWORDS	36

static int
mt7925_wtbl_idx_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;

	dev->wtbl_dump_idx = (u32)val;

	return 0;
}

static int
mt7925_wtbl_idx_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->wtbl_dump_idx;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_wtbl_idx, mt7925_wtbl_idx_get,
			 mt7925_wtbl_idx_set, "%llu\n");

static int
mt7925_wtbl_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u32 idx = dev->wtbl_dump_idx;
	u32 wdu_cr, wducr_val, base;
	u32 dw0, dw1;
	int i;

	if (!mt76_is_mmio(&dev->mt76)) {
		seq_puts(s, "WTBL direct dump not supported on this interface\n");
		return 0;
	}

	if (idx >= MT792x_WTBL_SIZE) {
		seq_printf(s, "Invalid WTBL idx %u (max %u)\n",
			   idx, MT792x_WTBL_SIZE - 1);
		return 0;
	}

	mt792x_mutex_acquire(dev);

	/* mt7925_mac_wtbl_lmac_addr() writes WDUCR group selection and
	 * returns the mapped register address for DW 0 of this WCID.
	 */
	base = mt7925_mac_wtbl_lmac_addr(dev, idx, 0);
	wdu_cr = is_mt7928(&dev->mt76) ? MT7928_WTBLON_TOP_WDUCR :
					  MT7925_WTBLON_TOP_WDUCR;
	wducr_val = mt76_rr(dev, wdu_cr);

	/* DW0[15:0] + DW1[31:0] encode the link address */
	dw0 = mt76_rr(dev, base);
	dw1 = mt76_rr(dev, base + 4);

	seq_printf(s, "Dump WTBL info of WLAN_IDX: %u\n", idx);
	seq_printf(s, "LMAC WTBL Addr: group:0x%08x=0x%08x addr: 0x%08x\n",
		   wdu_cr, wducr_val, base);
	seq_printf(s, "LinkAddr: %02x:%02x:%02x:%02x:%02x:%02x\n",
		   dw1 & 0xff, (dw1 >> 8) & 0xff,
		   (dw1 >> 16) & 0xff, (dw1 >> 24) & 0xff,
		   dw0 & 0xff, (dw0 >> 8) & 0xff);

	seq_puts(s, "\nLMAC WTBL raw data:\n");
	for (i = 0; i < MT792x_WTBL_LMAC_DWORDS; i++)
		seq_printf(s, "DW%02d: 0x%08x\n", i,
			   mt76_rr(dev, base + i * 4));

	mt792x_mutex_release(dev);

	return 0;
}

static int
mt7925_vefuse_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u16 size = dev->vefuse_cap.size;
	int off = 0;
	u8 *buf;
	int ret = 0;

	if (!dev->vefuse_cap.present || !size) {
		seq_puts(s, "vefuse: no region in FW image\n");
		return 0;
	}

	buf = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mt792x_mutex_acquire(dev);
	while (off < (int)size) {
		u16 chunk = min_t(u16, 512, size - off);

		ret = mt7925_mcu_read_vefuse(dev, off, chunk, buf + off);
		if (ret)
			break;
		off += chunk;
	}
	mt792x_mutex_release(dev);

	if (ret) {
		seq_printf(s, "vefuse: read failed at 0x%04x (%d)\n", off, ret);
		kfree(buf);
		return 0;
	}

	seq_printf(s, "vefuse content: %u bytes\n", size);
	for (off = 0; off < (int)size; off += 16)
		seq_printf(s, "%04x: %*ph\n", off,
			   (int)min_t(u16, 16, size - off), buf + off);

	kfree(buf);

	return 0;
}

/* send one perf_ind carrying the written value in every tx/rx byte field */
static int mt7925_perf_ind_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;

	return mt792x_perf_ind_trigger(dev, val);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_perf_ind, NULL, mt7925_perf_ind_set, "%lld\n");

static int mt7925_perf_ind_enable_get(void *data, u64 *val)
{
	struct mt792x_dev *dev = data;

	*val = dev->perf.enabled;

	return 0;
}

static int mt7925_perf_ind_enable_set(void *data, u64 val)
{
	struct mt792x_dev *dev = data;

	dev->perf.enabled = !!val;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_perf_ind_enable, mt7925_perf_ind_enable_get,
			 mt7925_perf_ind_enable_set, "%lld\n");

static int
mt7925_coex_info(struct seq_file *s, void *data)
{
#define MT7925_CHIP_CONFIG_RESP_SIZE	320
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u8 resp[MT7925_CHIP_CONFIG_RESP_SIZE], resp_type;
	int i, ret;

	mt792x_mutex_acquire(dev);
	ret = mt7925_mcu_chip_config_query(dev, "coexBwcGetModeInfo 0",
					   &resp_type, resp, sizeof(resp));
	mt792x_mutex_release(dev);

	if (ret < 0)
		return ret;

	if (!ret)
		seq_puts(s, "no reply\n");
	else if (resp_type == CHIP_CONFIG_TYPE_ASCII)
		seq_printf(s, "%.*s\n", ret, resp);
	else
		for (i = 0; i < ret; i += 16)
			seq_printf(s, "%04x: %*ph\n", i,
				   min_t(int, 16, ret - i), resp + i);

	return 0;
}

/* mt7925_dmashdl_read: standard DMASHDL (MT7925/MT7927 only).
 * MT7928 uses DMASHDL Lite; see mt7928_dmashdl_lite_read below.
 */
static int
mt7925_dmashdl_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u32 total_src = 0, total_rsv = 0;
	u32 ffa_cnt, free_pg_cnt;
	u32 ple_rsv, ple_src;
	u32 val;
	int i;

	if (!mt76_is_mmio(&dev->mt76)) {
		seq_puts(s, "DMASHDL dump not supported on this interface\n");
		return 0;
	}

	mt792x_mutex_acquire(dev);

	seq_puts(s, "DMASHDL Info:\n");

	val = mt76_rr(dev, MT_DMASHDL_REFILL);
	seq_printf(s, "\tRefill control(0x%08x): 0x%08x\n",
		   MT_DMASHDL_REFILL, val);

	val = mt76_rr(dev, MT_DMASHDL_PKT_MAX_SIZE);
	seq_printf(s, "\tPkt max size(0x%08x): 0x%08x\n",
		   MT_DMASHDL_PKT_MAX_SIZE, val);
	seq_printf(s, "\t\tPLE pkt max=0x%03x, PSE pkt max=0x%03x\n",
		   FIELD_GET(MT_DMASHDL_PKT_MAX_SIZE_PLE, val),
		   FIELD_GET(MT_DMASHDL_PKT_MAX_SIZE_PSE, val));

	val = mt76_rr(dev, MT_DMASHDL_ERROR_FLAG_CTRL);
	seq_printf(s, "\tError flag ctrl(0x%08x): 0x%08x\n",
		   MT_DMASHDL_ERROR_FLAG_CTRL, val);

	for (i = 0; i < 15; i++) {
		u32 quota = mt76_rr(dev, MT_DMASHDL_GROUP_QUOTA(i));
		u32 status = mt76_rr(dev, MT_DMASHDL_STATUS_RD_GP(i));
		u32 rsv = FIELD_GET(MT_DMASHDL_STATUS_RD_GP_RSV, status);
		u32 src = FIELD_GET(MT_DMASHDL_STATUS_RD_GP_SRC, status);

		seq_printf(s, "\tGroup %2d: quota(min/max)=0x%03x/0x%03x, used/rsv=0x%03x/0x%03x\n",
			   i,
			   FIELD_GET(MT_DMASHDL_GROUP_QUOTA_MIN, quota),
			   FIELD_GET(MT_DMASHDL_GROUP_QUOTA_MAX, quota),
			   src, rsv);
		total_src += src;
		total_rsv += rsv;
	}

	val = mt76_rr(dev, MT_DMASHDL_STATUS_RD);
	free_pg_cnt = FIELD_GET(MT_DMASHDL_STATUS_RD_FREE_PG, val);
	ffa_cnt = FIELD_GET(MT_DMASHDL_STATUS_RD_FFA, val);
	seq_printf(s, "\tStatus_RD(0x%08x): 0x%08x\n",
		   MT_DMASHDL_STATUS_RD, val);
	seq_printf(s, "\t\tfree page cnt=0x%03x, ffa cnt=0x%03x\n",
		   free_pg_cnt, ffa_cnt);

	/* Counter mismatch check against PLE HIF page counters */
	seq_puts(s, "\nDMASHDL Counter Check:\n");
	val = mt76_rr(dev, MT_PLE_HIF_PG_INFO);
	ple_rsv = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);
	ple_src = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);
	seq_printf(s, "\tPLE: used/rsv pages of HIF group=0x%03x/0x%03x\n",
		   ple_src, ple_rsv);
	seq_printf(s, "\tDMASHDL: total used pages of group0~14=0x%03x\n",
		   total_src);
	if (ple_src != total_src)
		seq_puts(s, "\t[MISMATCH] PLE used pages != DMASHDL total used\n");
	seq_printf(s, "\tDMASHDL: total rsv pages of group0~14=0x%03x\n",
		   total_rsv);
	seq_printf(s, "\tDMASHDL: total ffa pages=0x%03x, total free pages=0x%03x\n",
		   ffa_cnt, free_pg_cnt);
	if (free_pg_cnt != total_rsv + ffa_cnt)
		seq_puts(s, "\t[MISMATCH] free pages != rsv + ffa\n");
	if (free_pg_cnt != ple_rsv)
		seq_puts(s, "\t[MISMATCH] free pages != PLE rsv pages\n");
	if (ple_src == total_src && free_pg_cnt == total_rsv + ffa_cnt &&
	    free_pg_cnt == ple_rsv)
		seq_puts(s, "\tDMASHDL: no counter mismatch\n");

	mt792x_mutex_release(dev);
	return 0;
}

/* mt7928_dmashdl_lite_read: DMASHDL Lite (MT7928 only, base 0x20026200).
 * Unlike full DMASHDL, Lite splits runtime state into separate registers:
 *   GROUP_CONTROL (+0x100): min/max quota (config)
 *   GROUP_SRC_CNT (+0x200): pages sourced (in-use) per group (runtime)
 *   GROUP_ACK_CNT (+0x300): pages acknowledged (returned) per group (runtime)
 *   GROUP_VALID   (+0x400): bitmap of groups with active page allocations
 */
static int
mt7928_dmashdl_lite_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u32 disable0, disable1, valid0, valid1;
	u32 total_src = 0, total_ack = 0;
	u32 ple_rsv, ple_src;
	u32 val;
	int i;

	if (!mt76_is_mmio(&dev->mt76)) {
		seq_puts(s, "DMASHDL Lite dump not supported on this interface\n");
		return 0;
	}

	mt792x_mutex_acquire(dev);

	seq_puts(s, "DMASHDL Lite Info (MT7928):\n");

	val = mt76_rr(dev, MT_DMASHDL_LITE_MAIN_CONTROL);
	seq_printf(s, "\tMain control(0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_MAIN_CONTROL, val);

	val = mt76_rr(dev, MT_DMASHDL_LITE_PAGE_SIZE);
	seq_printf(s, "\tPage size(0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_PAGE_SIZE, val);
	seq_printf(s, "\t\tPSE page size=0x%04x, PLE page size=0x%04x\n",
		   FIELD_GET(MT_DMASHDL_LITE_PSE_PAGE_SIZE_MASK, val),
		   FIELD_GET(MT_DMASHDL_LITE_PLE_PAGE_SIZE_MASK, val));

	val = mt76_rr(dev, MT_DMASHDL_LITE_PKT_MAX_SIZE);
	seq_printf(s, "\tPkt max size(0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_PKT_MAX_SIZE, val);
	seq_printf(s, "\t\tPSE pkt max=0x%04x, PLE pkt max=0x%04x\n",
		   FIELD_GET(MT_DMASHDL_LITE_PSE_PKT_MAX_SIZE_MASK, val),
		   FIELD_GET(MT_DMASHDL_LITE_PLE_PKT_MAX_SIZE_MASK, val));

	val = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_SN_CHK0);
	seq_printf(s, "\tGroup SN check[31:0](0x%08x):  0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_SN_CHK0, val);
	val = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_SN_CHK1);
	seq_printf(s, "\tGroup SN check[63:32](0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_SN_CHK1, val);
	val = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_UDF_CHK0);
	seq_printf(s, "\tGroup UDF check[31:0](0x%08x):  0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_UDF_CHK0, val);
	val = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_UDF_CHK1);
	seq_printf(s, "\tGroup UDF check[63:32](0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_UDF_CHK1, val);

	disable0 = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_DISABLE0);
	seq_printf(s, "\tGroup disable[31:0](0x%08x):  0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_DISABLE0, disable0);
	disable1 = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_DISABLE1);
	seq_printf(s, "\tGroup disable[63:32](0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_DISABLE1, disable1);

	valid0 = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_VALID_0_31);
	seq_printf(s, "\tGroup valid[31:0](0x%08x):  0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_VALID_0_31, valid0);
	valid1 = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_VALID_32_63);
	seq_printf(s, "\tGroup valid[63:32](0x%08x): 0x%08x\n",
		   MT_DMASHDL_LITE_GROUP_VALID_32_63, valid1);

	seq_puts(s, "\tQ mapping (q2group):\n");
	for (i = 0; i < 8; i++) {
		val = mt76_rr(dev, MT_DMASHDL_LITE_Q_MAP(i));
		seq_printf(s, "\t\tQ_MAP[%d](0x%08x): 0x%08x\n",
			   i, MT_DMASHDL_LITE_Q_MAP(i), val);
	}

	seq_puts(s, "\tGroup status (skipping disabled+zero quota):\n");
	for (i = 0; i < DMASHDL_LITE_GROUP_NUM; i++) {
		bool enabled = !((i < 32 ? disable0 : disable1) >> (i % 32) & 1);
		u32 quota, src, ack_reg;
		u32 pktin, add_ret, ret;

		quota = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_QUOTA(i));
		if (!enabled && !quota)
			continue;

		src     = FIELD_GET(MT_DMASHDL_LITE_GROUP_SRC_CNT_MASK,
				    mt76_rr(dev, MT_DMASHDL_LITE_GROUP_SRC_CNT(i)));
		ack_reg = mt76_rr(dev, MT_DMASHDL_LITE_GROUP_ACK_CNT(i));
		pktin   = FIELD_GET(MT_DMASHDL_LITE_ACK_PKTIN_CNT,   ack_reg);
		add_ret = FIELD_GET(MT_DMASHDL_LITE_ACK_ADD_RET_CNT, ack_reg);
		ret     = FIELD_GET(MT_DMASHDL_LITE_ACK_RET_CNT,     ack_reg);

		seq_printf(s, "\tGroup %2d: quota(min/max)=0x%03x/0x%03x, src=0x%02x, ack(add_ret/pktin/ret)=0x%02x/0x%02x/0x%02x%s\n",
			   i,
			   FIELD_GET(MT_DMASHDL_LITE_GROUP_MIN_QUOTA_MASK, quota),
			   FIELD_GET(MT_DMASHDL_LITE_GROUP_MAX_QUOTA_MASK, quota),
			   src, add_ret, pktin, ret,
			   enabled ? "" : " [disabled]");
		if (enabled) {
			total_src += src;
			total_ack += ret;
		}
	}
	seq_printf(s, "\tTotal (enabled groups): src=0x%03x, ack_ret=0x%03x\n",
		   total_src, total_ack);

	/* Counter mismatch check against PLE HIF page counters */
	seq_puts(s, "\nDMASHDL Lite Counter Check:\n");
	val = mt76_rr(dev, MT_PLE_HIF_PG_INFO);
	ple_rsv = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);
	ple_src = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);
	seq_printf(s, "\tPLE: used/rsv pages of HIF group=0x%03x/0x%03x\n",
		   ple_src, ple_rsv);
	seq_printf(s, "\tDMASHDL Lite: total src=0x%03x, total ack_ret=0x%03x\n",
		   total_src, total_ack);
	if (ple_src != total_src)
		seq_puts(s, "\t[MISMATCH] PLE used pages != DMASHDL Lite total src\n");
	else
		seq_puts(s, "\tDMASHDL Lite: no counter mismatch\n");

	seq_puts(s, "\nPLE Page Counters:\n");

	val = mt76_rr(dev, MT_PLE_FREEPG_CNT);
	seq_printf(s, "\tPLE free pages(0x%08x): total=0x%03x, ffa=0x%03x\n",
		   MT_PLE_FREEPG_CNT,
		   FIELD_GET(MT_WF_FREEPG_FREE_CNT, val),
		   FIELD_GET(MT_WF_FREEPG_FFA_CNT, val));

	val = mt76_rr(dev, MT_PLE_HIF_WMTXD_PG_INFO);
	seq_printf(s, "\tPLE WMTXD group(0x%08x):     used/rsv=0x%03x/0x%03x\n",
		   MT_PLE_HIF_WMTXD_PG_INFO,
		   FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val),
		   FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val));

	val = mt76_rr(dev, MT_PLE_HIF_TXCMD_PG_INFO);
	seq_printf(s, "\tPLE HIF_TXCMD group(0x%08x): used/rsv=0x%03x/0x%03x\n",
		   MT_PLE_HIF_TXCMD_PG_INFO,
		   FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val),
		   FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val));

	val = mt76_rr(dev, MT_PLE_CPU_PG_INFO);
	seq_printf(s, "\tPLE CPU group(0x%08x):       used/rsv=0x%03x/0x%03x\n",
		   MT_PLE_CPU_PG_INFO,
		   FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val),
		   FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val));

	mt792x_mutex_release(dev);
	return 0;
}

static int
mt7925_ple_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u32 pbuf_ctrl, val;
	u32 fpg_cnt, ffa_cnt, fpg_head, fpg_tail;
	u32 max_q, min_q, rsv_pg, used_pg;

	if (!mt76_is_mmio(&dev->mt76)) {
		seq_puts(s, "PLE dump not supported on this interface\n");
		return 0;
	}

	mt792x_mutex_acquire(dev);

	pbuf_ctrl = mt76_rr(dev, MT_PLE_PBUF_CTRL);
	seq_puts(s, "PLE Configuration Info:\n");
	seq_printf(s, "\tPacket Buffer Control(0x%08x): 0x%08x\n",
		   MT_PLE_PBUF_CTRL, pbuf_ctrl);
	seq_printf(s, "\t\tPage Size=%d(%d bytes/page), Offset=%d, Total=%d pages\n",
		   (int)FIELD_GET(MT_WF_PBUF_PAGE_SIZE, pbuf_ctrl),
		   FIELD_GET(MT_WF_PBUF_PAGE_SIZE, pbuf_ctrl) ? 128 : 64,
		   (int)FIELD_GET(MT_WF_PBUF_PBUF_OFFSET, pbuf_ctrl),
		   (int)FIELD_GET(MT_WF_PBUF_TOTAL_PAGES, pbuf_ctrl));

	val = mt76_rr(dev, MT_PLE_INT_N9_ERR_STS);
	seq_printf(s, "\tINT_N9_ERR_STS(0x%08x): 0x%08x\n",
		   MT_PLE_INT_N9_ERR_STS, val);
	val = mt76_rr(dev, MT_PLE_INT_N9_ERR_STS_1);
	seq_printf(s, "\tINT_N9_ERR_STS_1(0x%08x): 0x%08x\n",
		   MT_PLE_INT_N9_ERR_STS_1, val);

	seq_puts(s, "PLE Page Flow Control:\n");

	val = mt76_rr(dev, MT_PLE_FREEPG_CNT);
	fpg_cnt = FIELD_GET(MT_WF_FREEPG_FREE_CNT, val);
	ffa_cnt = FIELD_GET(MT_WF_FREEPG_FFA_CNT, val);
	seq_printf(s, "\tFree page counter(0x%08x): 0x%08x\n",
		   MT_PLE_FREEPG_CNT, val);
	seq_printf(s, "\t\ttotal free=0x%03x, ffa=0x%03x\n", fpg_cnt, ffa_cnt);

	val = mt76_rr(dev, MT_PLE_FREEPG_HEAD_TAIL);
	fpg_head = FIELD_GET(MT_WF_FREEPG_HEAD, val);
	fpg_tail = FIELD_GET(MT_WF_FREEPG_TAIL, val);
	seq_printf(s, "\tFree page head/tail(0x%08x): 0x%08x\n",
		   MT_PLE_FREEPG_HEAD_TAIL, val);
	seq_printf(s, "\t\ttail/head=0x%03x/0x%03x\n", fpg_tail, fpg_head);

	/* HIF group */
	val = mt76_rr(dev, MT_PLE_PG_HIF_GROUP);
	max_q = FIELD_GET(MT_WF_GROUP_QUOTA_MAX, val);
	min_q = FIELD_GET(MT_WF_GROUP_QUOTA_MIN, val);
	seq_printf(s, "\tHIF group quota(0x%08x): 0x%08x (max/min=0x%03x/0x%03x)\n",
		   MT_PLE_PG_HIF_GROUP, val, max_q, min_q);
	val = mt76_rr(dev, MT_PLE_HIF_PG_INFO);
	used_pg = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);
	rsv_pg  = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);
	seq_printf(s, "\tHIF group status(0x%08x): 0x%08x (used/rsv=0x%03x/0x%03x)\n",
		   MT_PLE_HIF_PG_INFO, val, used_pg, rsv_pg);

	/* WMTXD group */
	val = mt76_rr(dev, MT_PLE_PG_HIF_WMTXD_GROUP);
	max_q = FIELD_GET(MT_WF_GROUP_QUOTA_MAX, val);
	min_q = FIELD_GET(MT_WF_GROUP_QUOTA_MIN, val);
	seq_printf(s, "\tWMTXD group quota(0x%08x): 0x%08x (max/min=0x%03x/0x%03x)\n",
		   MT_PLE_PG_HIF_WMTXD_GROUP, val, max_q, min_q);
	val = mt76_rr(dev, MT_PLE_HIF_WMTXD_PG_INFO);
	used_pg = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);
	rsv_pg  = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);
	seq_printf(s, "\tWMTXD group status(0x%08x): 0x%08x (used/rsv=0x%03x/0x%03x)\n",
		   MT_PLE_HIF_WMTXD_PG_INFO, val, used_pg, rsv_pg);

	/* HIF_TXCMD group */
	val = mt76_rr(dev, MT_PLE_PG_HIF_TXCMD_GROUP);
	max_q = FIELD_GET(MT_WF_GROUP_QUOTA_MAX, val);
	min_q = FIELD_GET(MT_WF_GROUP_QUOTA_MIN, val);
	seq_printf(s, "\tHIF_TXCMD group quota(0x%08x): 0x%08x (max/min=0x%03x/0x%03x)\n",
		   MT_PLE_PG_HIF_TXCMD_GROUP, val, max_q, min_q);
	val = mt76_rr(dev, MT_PLE_HIF_TXCMD_PG_INFO);
	used_pg = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);
	rsv_pg  = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);
	seq_printf(s, "\tHIF_TXCMD group status(0x%08x): 0x%08x (used/rsv=0x%03x/0x%03x)\n",
		   MT_PLE_HIF_TXCMD_PG_INFO, val, used_pg, rsv_pg);

	/* CPU group */
	val = mt76_rr(dev, MT_PLE_PG_CPU_GROUP);
	max_q = FIELD_GET(MT_WF_GROUP_QUOTA_MAX, val);
	min_q = FIELD_GET(MT_WF_GROUP_QUOTA_MIN, val);
	seq_printf(s, "\tCPU group quota(0x%08x): 0x%08x (max/min=0x%03x/0x%03x)\n",
		   MT_PLE_PG_CPU_GROUP, val, max_q, min_q);
	val = mt76_rr(dev, MT_PLE_CPU_PG_INFO);
	used_pg = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);
	rsv_pg  = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);
	seq_printf(s, "\tCPU group status(0x%08x): 0x%08x (used/rsv=0x%03x/0x%03x)\n",
		   MT_PLE_CPU_PG_INFO, val, used_pg, rsv_pg);

	val = mt76_rr(dev, MT_PLE_QUEUE_EMPTY);
	seq_printf(s, "\tQueue empty(0x%08x): 0x%08x%s\n",
		   MT_PLE_QUEUE_EMPTY, val,
		   (val & MT_PLE_QUEUE_EMPTY_ALL_AC) ? " [ALL AC EMPTY]" : "");

	mt792x_mutex_release(dev);
	return 0;
}

static int
mt7925_pse_read(struct seq_file *s, void *data)
{
	struct mt792x_dev *dev = dev_get_drvdata(s->private);
	u32 pbuf_ctrl, val;
	u32 fpg_cnt, ffa_cnt, fpg_head, fpg_tail;
	u32 max_q, min_q, rsv_pg, used_pg;

	if (!mt76_is_mmio(&dev->mt76)) {
		seq_puts(s, "PSE dump not supported on this interface\n");
		return 0;
	}

	mt792x_mutex_acquire(dev);

	pbuf_ctrl = mt76_rr(dev, MT_PSE_PBUF_CTRL);
	seq_puts(s, "PSE Configuration Info:\n");
	seq_printf(s, "\tPacket Buffer Control(0x%08x): 0x%08x\n",
		   MT_PSE_PBUF_CTRL, pbuf_ctrl);
	seq_printf(s, "\t\tPage Size=%d(%d bytes/page), Offset=%d, Total=%d pages\n",
		   (int)FIELD_GET(MT_WF_PBUF_PAGE_SIZE, pbuf_ctrl),
		   FIELD_GET(MT_WF_PBUF_PAGE_SIZE, pbuf_ctrl) ? 256 : 128,
		   (int)FIELD_GET(MT_WF_PBUF_PBUF_OFFSET, pbuf_ctrl),
		   (int)FIELD_GET(MT_WF_PBUF_TOTAL_PAGES, pbuf_ctrl));

	val = mt76_rr(dev, MT_PSE_INT_N9_ERR_STS);
	seq_printf(s, "\tINT_N9_ERR_STS(0x%08x): 0x%08x\n",
		   MT_PSE_INT_N9_ERR_STS, val);
	val = mt76_rr(dev, MT_PSE_INT_N9_ERR1_STS);
	seq_printf(s, "\tINT_N9_ERR1_STS(0x%08x): 0x%08x\n",
		   MT_PSE_INT_N9_ERR1_STS, val);

	val = mt76_rr(dev, MT_PSE_QUEUE_EMPTY);
	seq_printf(s, "\tQueue empty(0x%08x): 0x%08x\n",
		   MT_PSE_QUEUE_EMPTY, val);

	seq_puts(s, "PSE Page Flow Control:\n");

	val = mt76_rr(dev, MT_PSE_FREEPG_CNT);
	fpg_cnt = FIELD_GET(MT_WF_FREEPG_FREE_CNT, val);
	ffa_cnt = FIELD_GET(MT_WF_FREEPG_FFA_CNT, val);
	seq_printf(s, "\tFree page counter(0x%08x): 0x%08x\n",
		   MT_PSE_FREEPG_CNT, val);
	seq_printf(s, "\t\ttotal free=0x%03x, ffa=0x%03x\n", fpg_cnt, ffa_cnt);

	val = mt76_rr(dev, MT_PSE_FREEPG_HEAD_TAIL);
	fpg_head = FIELD_GET(MT_WF_FREEPG_HEAD, val);
	fpg_tail = FIELD_GET(MT_WF_FREEPG_TAIL, val);
	seq_printf(s, "\tFree page head/tail(0x%08x): 0x%08x\n",
		   MT_PSE_FREEPG_HEAD_TAIL, val);
	seq_printf(s, "\t\ttail/head=0x%03x/0x%03x\n", fpg_tail, fpg_head);

#define MT7925_PSE_GROUP(_name, _qreg, _ireg)					\
do {										\
	val = mt76_rr(dev, _qreg);						\
	max_q = FIELD_GET(MT_WF_GROUP_QUOTA_MAX, val);				\
	min_q = FIELD_GET(MT_WF_GROUP_QUOTA_MIN, val);				\
	seq_printf(s, "\t" _name " group quota(0x%08x): 0x%08x"		\
		   " (max/min=0x%03x/0x%03x)\n", _qreg, val, max_q, min_q);	\
	val = mt76_rr(dev, _ireg);						\
	used_pg = FIELD_GET(MT_WF_PG_INFO_SRC_CNT, val);			\
	rsv_pg  = FIELD_GET(MT_WF_PG_INFO_RSV_CNT, val);			\
	seq_printf(s, "\t" _name " group status(0x%08x): 0x%08x"		\
		   " (used/rsv=0x%03x/0x%03x)\n", _ireg, val, used_pg, rsv_pg);\
} while (0)

	MT7925_PSE_GROUP("HIF0",  MT_PSE_PG_HIF0_GROUP,  MT_PSE_HIF0_PG_INFO);
	MT7925_PSE_GROUP("HIF1",  MT_PSE_PG_HIF1_GROUP,  MT_PSE_HIF1_PG_INFO);
	MT7925_PSE_GROUP("HIF2",  MT_PSE_PG_HIF2_GROUP,  MT_PSE_HIF2_PG_INFO);
	MT7925_PSE_GROUP("CPU",   MT_PSE_PG_CPU_GROUP,   MT_PSE_CPU_PG_INFO);
	MT7925_PSE_GROUP("PLE",   MT_PSE_PG_PLE_GROUP,   MT_PSE_PLE_PG_INFO);
	MT7925_PSE_GROUP("LMAC0", MT_PSE_PG_LMAC0_GROUP, MT_PSE_LMAC0_PG_INFO);
	MT7925_PSE_GROUP("LMAC1", MT_PSE_PG_LMAC1_GROUP, MT_PSE_LMAC1_PG_INFO);
	MT7925_PSE_GROUP("LMAC2", MT_PSE_PG_LMAC2_GROUP, MT_PSE_LMAC2_PG_INFO);
	MT7925_PSE_GROUP("LMAC3", MT_PSE_PG_LMAC3_GROUP, MT_PSE_LMAC3_PG_INFO);
	MT7925_PSE_GROUP("MDP",   MT_PSE_PG_MDP_GROUP,   MT_PSE_MDP_PG_INFO);

#undef MT7925_PSE_GROUP

	mt792x_mutex_release(dev);
	return 0;
}

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
	tx_per = tx_total == 0 ? 0 : (1000 * (tx_fail)) / tx_total;
	seq_printf(s, "%-22s  AMPDU Tx fail count   = %llu  (MPDU), PER=%llu.%1llu%%\n",
		   " ",
		   tx_fail,
		   tx_per / 10, tx_per % 10);

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
