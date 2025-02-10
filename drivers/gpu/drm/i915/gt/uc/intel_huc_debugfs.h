/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2020 Intel Corporation
 */

#ifndef DEBUGFS_HUC_H
#define DEBUGFS_HUC_H

struct intel_huc;
struct dentry;
struct debugfs_node;

void intel_huc_debugfs_register(struct intel_huc *huc,
				struct debugfs_node *root);

#endif /* DEBUGFS_HUC_H */
