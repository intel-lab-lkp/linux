/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2024, Advanced Micro Devices, Inc.
 */

#ifndef _AMDXDNA_DEBUGFS_H_
#define _AMDXDNA_DEBUGFS_H_

struct amdxdna_dev;

#ifdef CONFIG_DEBUG_FS
void amdxdna_debugfs_init(struct amdxdna_dev *xdna);
#else
static inline void amdxdna_debugfs_init(struct amdxdna_dev *xdna) {}
#endif

#endif /* _AMDXDNA_DEBUGFS_H_ */
