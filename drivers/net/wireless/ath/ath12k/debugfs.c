// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include "core.h"
#include "debug.h"
#include "debugfs.h"
#include "debugfs_htt_stats.h"

static ssize_t ath12k_write_simulate_radar(struct file *file,
					   const char __user *user_buf,
					   size_t count, loff_t *ppos)
{
	struct ath12k *ar = file->private_data;
	int ret;

	wiphy_lock(ath12k_ar_to_hw(ar)->wiphy);
	ret = ath12k_wmi_simulate_radar(ar);
	if (ret)
		goto exit;

	ret = count;
exit:
	wiphy_unlock(ath12k_ar_to_hw(ar)->wiphy);
	return ret;
}

static const struct file_operations fops_simulate_radar = {
	.write = ath12k_write_simulate_radar,
	.open = simple_open
};

void ath12k_debugfs_soc_create(struct ath12k_base *ab)
{
	bool dput_needed;
	char soc_name[64] = { 0 };
	struct dentry *debugfs_ath12k;

	debugfs_ath12k = debugfs_lookup("ath12k", NULL);
	if (debugfs_ath12k) {
		/* a dentry from lookup() needs dput() after we don't use it */
		dput_needed = true;
	} else {
		debugfs_ath12k = debugfs_create_dir("ath12k", NULL);
		if (IS_ERR_OR_NULL(debugfs_ath12k))
			return;
		dput_needed = false;
	}

	scnprintf(soc_name, sizeof(soc_name), "%s-%s", ath12k_bus_str(ab->hif.bus),
		  dev_name(ab->dev));

	ab->debugfs_soc = debugfs_create_dir(soc_name, debugfs_ath12k);

	if (dput_needed)
		dput(debugfs_ath12k);
}

void ath12k_debugfs_soc_destroy(struct ath12k_base *ab)
{
	debugfs_remove_recursive(ab->debugfs_soc);
	ab->debugfs_soc = NULL;
	/* We are not removing ath12k directory on purpose, even if it
	 * would be empty. This simplifies the directory handling and it's
	 * a minor cosmetic issue to leave an empty ath12k directory to
	 * debugfs.
	 */
}

static ssize_t ath12k_write_wmi_ctrl_path_stats(struct file *file,
						const char __user *ubuf,
						size_t count, loff_t *ppos)
{
	struct ath12k *ar = file->private_data;
	struct wmi_ctrl_path_stats_arg arg = {};
	struct ath12k_hw *ah = ath12k_ar_to_ah(ar);
	u8 buf[128] = {0};
	int ret;

	ret = simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, ubuf, count);
	if (ret < 0)
		return ret;

	buf[ret] = '\0';

	ret = sscanf(buf, "%u %u", &arg.stats_id, &arg.action);
	if (ret != 2)
		return -EINVAL;

	if (!arg.action || arg.action > WMI_REQUEST_CTRL_PATH_STAT_RESET)
		return -EINVAL;

	guard(mutex)(&ah->hw_mutex);
	ret = ath12k_wmi_send_wmi_ctrl_stats_cmd(ar, &arg);
	if (ret && ret != -ETIMEDOUT) {
		ath12k_info(ar->ab, "failed to send ctrl path stats request %d\n",
			    ret);
		return ret;
	}

	return count;
}

static int wmi_ctrl_path_pdev_stat(struct ath12k *ar, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	char fw_tx_mgmt_subtype[WMI_MAX_STRING_LEN] = {0};
	char fw_rx_mgmt_subtype[WMI_MAX_STRING_LEN] = {0};
	struct wmi_ctrl_path_pdev_stats *stats, *tmp;
	u16 index_tx, index_rx;
	const int size = 2048;
	u8 i;
	int len = 0;

	char *buf __free(kfree) = kzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	LIST_HEAD(wmi_stats_list);

	spin_lock_bh(&ar->debug.wmi_ctrl_path_stats_lock);
	list_splice_tail_init(&ar->debug.wmi_ctrl_path_stats.pdev_stats, &wmi_stats_list);
	spin_unlock_bh(&ar->debug.wmi_ctrl_path_stats_lock);

	list_for_each_entry_safe(stats, tmp, &wmi_stats_list, list) {
		if (!stats)
			break;

		index_tx = 0;
		index_rx = 0;

		for (i = 0; i < IEEE80211_MGMT_FRAME_SUBTYPE_MAX; i++) {
			index_tx += scnprintf(&fw_tx_mgmt_subtype[index_tx],
					     WMI_MAX_STRING_LEN - index_tx,
					     " %u:%u,", i,
					    stats->tx_mgmt_subtype[i]);
			index_rx += scnprintf(&fw_rx_mgmt_subtype[index_rx],
					     WMI_MAX_STRING_LEN - index_rx,
					     " %u:%u,", i,
					     stats->rx_mgmt_subtype[i]);
		}

		len += scnprintf(buf + len, size - len,
				 "WMI_CTRL_PATH_PDEV_TX_STATS:\n");
		len += scnprintf(buf + len, size - len,
				 "fw_tx_mgmt_subtype = %s\n",
				 fw_tx_mgmt_subtype);
		len += scnprintf(buf + len, size - len,
				 "fw_rx_mgmt_subtype = %s\n",
				 fw_rx_mgmt_subtype);
		len += scnprintf(buf + len, size - len,
				 "scan_fail_dfs_violation_time_ms = %u\n",
				 stats->scan_fail_dfs_viol_time_ms);
		len += scnprintf(buf + len, size - len,
				 "nol_chk_fail_last_chan_freq = %u\n",
				 stats->nol_chk_fail_last_chan_freq);
		len += scnprintf(buf + len, size - len,
				 "nol_chk_fail_time_stamp_ms = %u\n",
				 stats->nol_chk_fail_time_stamp_ms);
		len += scnprintf(buf + len, size - len,
				 "tot_peer_create_cnt = %u\n",
				 stats->tot_peer_create_cnt);
		len += scnprintf(buf + len, size - len,
				 "tot_peer_del_cnt = %u\n",
				 stats->tot_peer_del_cnt);
		len += scnprintf(buf + len, size - len,
				 "tot_peer_del_resp_cnt = %u\n",
				 stats->tot_peer_del_resp_cnt);
		len += scnprintf(buf + len, size - len,
				 "vdev_pause_fail_rt_to_sched_algo_fifo_full_cnt = %u\n",
				 stats->sched_algo_fifo_full_cnt);
		list_del(&stats->list);
		kfree(stats);
	}

	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static ssize_t ath12k_read_wmi_ctrl_path_stats(struct file *file,
					       char __user *ubuf,
					       size_t count, loff_t *ppos)
{
	struct ath12k *ar = file->private_data;
	int ret;
	enum wmi_tlv_tag tagid;

	tagid = ar->debug.wmi_ctrl_path_stats_tagid;

	switch (tagid) {
	case WMI_TAG_CTRL_PATH_PDEV_STATS:
		ret = wmi_ctrl_path_pdev_stat(ar, ubuf, count, ppos);
		break;
	default:
		/* Unsupported tag */
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct file_operations ath12k_fops_wmi_ctrl_stats = {
	.write = ath12k_write_wmi_ctrl_path_stats,
	.open = simple_open,
	.read = ath12k_read_wmi_ctrl_path_stats,
};

static void ath12k_debugfs_wmi_ctrl_stats_register(struct ath12k *ar)
{
	debugfs_create_file("wmi_ctrl_stats", 0600,
			    ar->debug.debugfs_pdev,
			    ar,
			    &ath12k_fops_wmi_ctrl_stats);
	INIT_LIST_HEAD(&ar->debug.wmi_ctrl_path_stats.pdev_stats);
	spin_lock_init(&ar->debug.wmi_ctrl_path_stats_lock);
	init_completion(&ar->debug.wmi_ctrl_path_stats_rcvd);
	ar->debug.wmi_ctrl_path_stats_more_enabled = false;
}

void ath12k_debugfs_register(struct ath12k *ar)
{
	struct ath12k_base *ab = ar->ab;
	struct ieee80211_hw *hw = ar->ah->hw;
	char pdev_name[5];
	char buf[100] = {0};

	scnprintf(pdev_name, sizeof(pdev_name), "%s%d", "mac", ar->pdev_idx);

	ar->debug.debugfs_pdev = debugfs_create_dir(pdev_name, ab->debugfs_soc);

	/* Create a symlink under ieee80211/phy* */
	scnprintf(buf, sizeof(buf), "../../ath12k/%pd2", ar->debug.debugfs_pdev);
	ar->debug.debugfs_pdev_symlink = debugfs_create_symlink("ath12k",
								hw->wiphy->debugfsdir,
								buf);

	if (ar->mac.sbands[NL80211_BAND_5GHZ].channels) {
		debugfs_create_file("dfs_simulate_radar", 0200,
				    ar->debug.debugfs_pdev, ar,
				    &fops_simulate_radar);
	}

	ath12k_debugfs_htt_stats_register(ar);

	if (test_bit(WMI_TLV_SERVICE_CTRL_PATH_STATS_REQUEST,
		     ar->ab->wmi_ab.svc_map))
		ath12k_debugfs_wmi_ctrl_stats_register(ar);

}

void ath12k_debugfs_unregister(struct ath12k *ar)
{
	if (!ar->debug.debugfs_pdev)
		return;

	/* Remove symlink under ieee80211/phy* */
	debugfs_remove(ar->debug.debugfs_pdev_symlink);
	debugfs_remove_recursive(ar->debug.debugfs_pdev);
	ar->debug.debugfs_pdev_symlink = NULL;
	ar->debug.debugfs_pdev = NULL;
}
