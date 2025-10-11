/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SRIOV_PF_MIGRATION_H_
#define _XE_SRIOV_PF_MIGRATION_H_

#include <linux/types.h>

struct xe_device;

#ifdef CONFIG_PCI_IOV
int xe_sriov_pf_migration_init(struct xe_device *xe);
bool xe_sriov_pf_migration_supported(struct xe_device *xe);
#else
static inline int xe_sriov_pf_migration_init(struct xe_device *xe)
{
	return 0;
}
static inline bool xe_sriov_pf_migration_supported(struct xe_device *xe)
{
	return false;
}
#endif

#endif
