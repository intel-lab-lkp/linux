/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SRIOV_PF_MIGRATION_H_
#define _XE_SRIOV_PF_MIGRATION_H_

#include <linux/types.h>
#include <linux/wait.h>

struct xe_device;

#ifdef CONFIG_PCI_IOV
int xe_sriov_pf_migration_init(struct xe_device *xe);
bool xe_sriov_pf_migration_supported(struct xe_device *xe);
ssize_t xe_sriov_pf_migration_size(struct xe_device *xe, unsigned int vfid);
struct xe_sriov_pf_migration_data *
xe_sriov_pf_migration_consume(struct xe_device *xe, unsigned int vfid);
int xe_sriov_pf_migration_produce(struct xe_device *xe, unsigned int vfid,
				  struct xe_sriov_pf_migration_data *data);
wait_queue_head_t *xe_sriov_pf_migration_waitqueue(struct xe_device *xe, unsigned int vfid);
#else
static inline int xe_sriov_pf_migration_init(struct xe_device *xe)
{
	return 0;
}
static inline bool xe_sriov_pf_migration_supported(struct xe_device *xe)
{
	return false;
}
static inline struct xe_sriov_pf_migration_data *
xe_sriov_pf_migration_consume(struct xe_device *xe, unsigned int vfid)
{
	return ERR_PTR(-ENODEV);
}
static inline int xe_sriov_pf_migration_produce(struct xe_device *xe, unsigned int vfid,
						struct xe_sriov_pf_migration_data *data)
{
	return -ENODEV;
}
wait_queue_head_t *xe_sriov_pf_migration_waitqueue(struct xe_device *xe, unsigned int vfid)
{
	return ERR_PTR(-ENODEV);
}
#endif

#endif
