/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_GT_SRIOV_PF_MIGRATION_TYPES_H_
#define _XE_GT_SRIOV_PF_MIGRATION_TYPES_H_

#include <linux/ptr_ring.h>

/**
 * struct xe_gt_sriov_migration_data - GT-level per-VF migration data.
 *
 * Used by the PF driver to maintain per-VF migration data.
 */
struct xe_gt_sriov_migration_data {
	/** @ring: queue containing VF save / restore migration data */
	struct ptr_ring ring;

	/** @vram_save_offset: offset within VRAM, used for chunked VRAM save */
	loff_t vram_save_offset;
};

#endif
