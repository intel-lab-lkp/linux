/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef DEBUGFS_GUC_LOG_H
#define DEBUGFS_GUC_LOG_H

struct intel_guc_log;
struct dentry;
struct debugfs_node;

void intel_guc_log_debugfs_register(struct intel_guc_log *log,
				    struct debugfs_node *root);

#endif /* DEBUGFS_GUC_LOG_H */
