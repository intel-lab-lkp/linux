/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GGTT_TYPES_H_
#define _XE_GGTT_TYPES_H_

#include <drm/drm_mm.h>

#include "xe_pt_types.h"

struct xe_bo;
struct xe_gt;

/**
 * struct xe_ggtt_node - A node in GGTT.
 *
 * This struct needs to be initialized (only-once) with xe_ggtt_node_init() before any node
 * insertion, reservation, or 'ballooning'.
 * It will, then, be finalized by either xe_ggtt_node_remove() or xe_ggtt_node_deballoon().
 */
struct xe_ggtt_node {
	/** @ggtt: Back pointer to xe_ggtt where this region will be inserted at */
	struct xe_ggtt *ggtt;
	/** @base: A drm_mm_node */
	struct drm_mm_node base;
	/** @delayed_removal_work: The work struct for the delayed removal */
	struct work_struct delayed_removal_work;
	/** @invalidate_on_remove: If it needs invalidation upon removal */
	bool invalidate_on_remove;
};

typedef void (*xe_ggtt_set_pte_fn)(struct xe_ggtt *ggtt, u64 addr, u64 pte);
typedef void (*xe_ggtt_transform_cb)(struct xe_ggtt *ggtt,
				     struct xe_ggtt_node *node,
				     u64 pte_flags,
				     xe_ggtt_set_pte_fn set_pte, void *arg);

#endif
