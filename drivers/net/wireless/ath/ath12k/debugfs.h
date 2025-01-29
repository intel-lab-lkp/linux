/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2018-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _ATH12K_DEBUGFS_H_
#define _ATH12K_DEBUGFS_H_

#ifdef CONFIG_ATH12K_DEBUGFS
void ath12k_debugfs_soc_create(struct ath12k_base *ab);
void ath12k_debugfs_soc_destroy(struct ath12k_base *ab);
void ath12k_debugfs_register(struct ath12k *ar);
void ath12k_debugfs_unregister(struct ath12k *ar);
void ath12k_debugfs_fw_stats_process(struct ath12k *ar,
				     struct ath12k_fw_stats *stats);
void ath12k_debugfs_fw_stats_reset(struct ath12k *ar);

static inline bool ath12k_debugfs_is_extd_rx_stats_enabled(struct ath12k *ar)
{
	return ar->debug.extd_rx_stats;
}

static inline int ath12k_debugfs_rx_filter(struct ath12k *ar)
{
	return ar->debug.rx_filter;
}
#else
static inline void ath12k_debugfs_soc_create(struct ath12k_base *ab)
{
}

static inline void ath12k_debugfs_soc_destroy(struct ath12k_base *ab)
{
}

static inline void ath12k_debugfs_register(struct ath12k *ar)
{
}

static inline void ath12k_debugfs_unregister(struct ath12k *ar)
{
}

static inline void ath12k_debugfs_fw_stats_process(struct ath12k *ar,
						   struct ath12k_fw_stats *stats)
{
}

static inline void ath12k_debugfs_fw_stats_reset(struct ath12k *ar)
{
}

static inline bool ath12k_debugfs_is_extd_rx_stats_enabled(struct ath12k *ar)
{
	return false;
}

static inline int ath12k_debugfs_rx_filter(struct ath12k *ar)
{
	return 0;
}
#endif /* CONFIG_ATH12K_DEBUGFS */

#endif /* _ATH12K_DEBUGFS_H_ */
