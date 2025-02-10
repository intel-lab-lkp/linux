/* SPDX-License-Identifier: MIT */

/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef INTEL_SSEU_DEBUGFS_H
#define INTEL_SSEU_DEBUGFS_H

struct intel_gt;
struct dentry;
struct debugfs_node;
struct seq_file;

int intel_sseu_status(struct seq_file *m, struct intel_gt *gt);
void intel_sseu_debugfs_register(struct intel_gt *gt,
				 struct debugfs_node *root);

#endif /* INTEL_SSEU_DEBUGFS_H */
