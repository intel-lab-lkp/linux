/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2024 Intel Corporation
 */

#ifndef _XE_GT_SRIOV_PF_MIGRATION_H_
#define _XE_GT_SRIOV_PF_MIGRATION_H_

#include <linux/types.h>

struct xe_gt;
struct xe_sriov_pf_migration_data;

int xe_gt_sriov_pf_migration_init(struct xe_gt *gt);
ssize_t xe_gt_sriov_pf_migration_guc_size(struct xe_gt *gt, unsigned int vfid);
int xe_gt_sriov_pf_migration_guc_save(struct xe_gt *gt, unsigned int vfid);
int xe_gt_sriov_pf_migration_guc_restore(struct xe_gt *gt, unsigned int vfid,
					 struct xe_sriov_pf_migration_data *data);
ssize_t xe_gt_sriov_pf_migration_ggtt_size(struct xe_gt *gt, unsigned int vfid);
int xe_gt_sriov_pf_migration_ggtt_save(struct xe_gt *gt, unsigned int vfid);
int xe_gt_sriov_pf_migration_ggtt_restore(struct xe_gt *gt, unsigned int vfid,
					  struct xe_sriov_pf_migration_data *data);

ssize_t xe_gt_sriov_pf_migration_size(struct xe_gt *gt, unsigned int vfid);

bool xe_gt_sriov_pf_migration_ring_empty(struct xe_gt *gt, unsigned int vfid);
int xe_gt_sriov_pf_migration_ring_produce(struct xe_gt *gt, unsigned int vfid,
					  struct xe_sriov_pf_migration_data *data);
struct xe_sriov_pf_migration_data *
xe_gt_sriov_pf_migration_ring_consume(struct xe_gt *gt, unsigned int vfid);
struct xe_sriov_pf_migration_data *
xe_gt_sriov_pf_migration_ring_consume_nowait(struct xe_gt *gt, unsigned int vfid);

#endif
