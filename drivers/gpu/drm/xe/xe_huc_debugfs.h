/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_HUC_DEBUGFS_H_
#define _XE_HUC_DEBUGFS_H_

struct dentry;
struct debugfs_node;
struct xe_huc;

void xe_huc_debugfs_register(struct xe_huc *huc, struct debugfs_node *parent);

#endif
