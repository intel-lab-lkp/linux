/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2025 Google LLC */

#ifndef __PANTHOR_SOC_H__
#define __PANTHOR_SOC_H__

#include <linux/types.h>

struct panthor_device;

/**
 * struct panthor_soc_data - Panthor SoC Data
 */
struct panthor_soc_data {
	/** @asn_hash_enable: True if GPU_L2_CONFIG_ASN_HASH_ENABLE must be set. */
	bool asn_hash_enable;

	/** @asn_hash: ASN_HASH values when asn_hash_enable is true. */
	u32 asn_hash[3];
};

#ifdef CONFIG_DRM_PANTHOR_SOC_MT8196
extern const struct panthor_soc_data panthor_soc_data_mediatek_mt8196;
#endif

#endif /* __PANTHOR_SOC_H__ */
