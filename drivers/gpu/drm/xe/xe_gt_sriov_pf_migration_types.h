/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_GT_SRIOV_PF_MIGRATION_TYPES_H_
#define _XE_GT_SRIOV_PF_MIGRATION_TYPES_H_

#include <linux/ptr_ring.h>

/**
 * struct xe_gt_sriov_pf_migration - GT-level data.
 *
 * Used by the PF driver to maintain per-VF migration data.
 */
struct xe_gt_sriov_pf_migration {
	/** @ring: queue containing VF save / restore migration data */
	struct ptr_ring ring;
};

#endif
