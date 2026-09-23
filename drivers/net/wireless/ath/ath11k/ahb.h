/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#ifndef ATH11K_AHB_H
#define ATH11K_AHB_H

#include <linux/remoteproc/qcom_rproc.h>

#include "core.h"

#define ATH11K_AHB_RECOVERY_TIMEOUT (3 * HZ)

#define ATH11K_AHB_SMP2P_SMEM_MSG		GENMASK(15, 0)
#define ATH11K_AHB_SMP2P_SMEM_SEQ_NO		GENMASK(31, 16)
#define ATH11K_AHB_SMP2P_SMEM_VALUE_MASK	0xFFFFFFFF

#define ATH11K_ROOTPD_READY_TIMEOUT		(5 * HZ)
#define ATH11K_RPROC_AFTER_POWERUP		QCOM_SSR_AFTER_POWERUP

enum ath11k_ahb_smp2p_msg_id {
	ATH11K_AHB_POWER_SAVE_ENTER = 1,
	ATH11K_AHB_POWER_SAVE_EXIT,
};

struct ath11k_base;

struct ath11k_ahb_rproc_info {
	struct rproc *tgt_rproc;

	struct completion rootpd_ready;
	struct notifier_block root_pd_nb;
	void *root_pd_notifier;
	bool root_pd_booted;

	/* Bitmap of loaded M3 firmwares indexed by hardware revision */
	u32 m3_loaded;
};

struct ath11k_ahb {
	struct ath11k_base *ab;
	struct {
		struct device *dev;
		struct iommu_domain *iommu_domain;
		dma_addr_t msa_paddr;
		u32 msa_size;
		dma_addr_t ce_paddr;
		u32 ce_size;
		bool use_tz;
	} fw;
	struct {
		unsigned short seq_no;
		unsigned int smem_bit;
		struct qcom_smem_state *smem_state;
	} smp2p_info;
	struct ath11k_ahb_rproc_info *rproc_info;
};

static inline struct ath11k_ahb *ath11k_ahb_priv(struct ath11k_base *ab)
{
	return (struct ath11k_ahb *)ab->drv_priv;
}
#endif
