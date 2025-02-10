/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef DEBUGFS_UC_H
#define DEBUGFS_UC_H

struct intel_uc;
struct dentry;
#define debugfs_node dentry

void intel_uc_debugfs_register(struct intel_uc *uc,
			       struct debugfs_node *gt_root);

#endif /* DEBUGFS_UC_H */
