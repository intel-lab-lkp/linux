/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef DEBUGFS_GSC_UC_H
#define DEBUGFS_GSC_UC_H

struct intel_gsc_uc;
struct dentry;
struct debugfs_node;

void intel_gsc_uc_debugfs_register(struct intel_gsc_uc *gsc,
				   struct debugfs_node *root);

#endif /* DEBUGFS_GSC_UC_H */
