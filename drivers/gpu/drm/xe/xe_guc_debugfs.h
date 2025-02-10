/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_DEBUGFS_H_
#define _XE_GUC_DEBUGFS_H_

struct dentry;
struct debugfs_node;
struct xe_guc;

void xe_guc_debugfs_register(struct xe_guc *guc, struct debugfs_node *parent);

#endif
