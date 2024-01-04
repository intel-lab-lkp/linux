/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef INTEL_GT_CCS_MODE_H
#define INTEL_GT_CCS_MODE_H

struct intel_gt;

void intel_gt_init_ccs_mode(struct intel_gt *gt);
void intel_gt_fini_ccs_mode(struct intel_gt *gt);

void intel_gt_apply_ccs_mode(struct intel_gt *gt);
void intel_gt_sysfs_ccs_mode(struct intel_gt *gt);

#endif /* INTEL_GT_CCS_MODE_H */
