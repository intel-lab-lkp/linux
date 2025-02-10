/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023-2024 Intel Corporation
 */

#ifndef _XE_GT_SRIOV_PF_DEBUGFS_H_
#define _XE_GT_SRIOV_PF_DEBUGFS_H_

struct xe_gt;
struct dentry;
#define debugfs_node dentry

#ifdef CONFIG_PCI_IOV
void xe_gt_sriov_pf_debugfs_register(struct xe_gt *gt,
				     struct debugfs_node *root);
#else
static inline void xe_gt_sriov_pf_debugfs_register(struct xe_gt *gt,
						   struct debugfs_node *root) { }
#endif

#endif
