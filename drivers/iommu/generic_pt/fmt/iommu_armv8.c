// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024-2026, NVIDIA CORPORATION & AFFILIATES
 */
#define PT_FMT armv8
#define PT_SUPPORTED_FEATURES                                  \
	(BIT(PT_FEAT_DMA_INCOHERENT) | BIT(PT_FEAT_ARMV8_S2) | \
	 BIT(PT_FEAT_ARMV8_LVA) |                              \
	 BIT(PT_FEAT_ARMV8_DBM) | BIT(PT_FEAT_ARMV8_S2FWB) |  \
	 BIT(PT_FEAT_DETAILED_GATHER))
#define PT_FORCE_ENABLED_FEATURES BIT(PT_FEAT_DETAILED_GATHER)

#include "iommu_template.h"
