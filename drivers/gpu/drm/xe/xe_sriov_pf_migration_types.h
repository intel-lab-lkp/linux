/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SRIOV_PF_MIGRATION_TYPES_H_
#define _XE_SRIOV_PF_MIGRATION_TYPES_H_

#include <linux/types.h>
#include <linux/mutex_types.h>
#include <linux/wait.h>

struct xe_sriov_pf_migration_data {
	struct xe_device *xe;
	void *vaddr;
	size_t remaining;
	size_t hdr_remaining;
	union {
		struct xe_bo *bo;
		void *buff;
	};
	__struct_group(xe_sriov_pf_migration_hdr, hdr, __packed,
		u8 version;
		u8 type;
		u8 tile;
		u8 gt;
		u32 flags;
		u64 offset;
		u64 size;
	);
};

struct xe_sriov_pf_migration {
	/** @wq: waitqueue used to avoid busy-waiting for snapshot production/consumption */
	wait_queue_head_t wq;
	/** @lock: Mutex protecting the migration data */
	struct mutex lock;
	/** @pending: currently processed data packet of VF resource */
	struct xe_sriov_pf_migration_data *pending;
	/** @trailer: data packet used to indicate the end of stream */
	struct xe_sriov_pf_migration_data *trailer;
	/** @descriptor: data packet containing the metadata describing the device */
	struct xe_sriov_pf_migration_data *descriptor;
};

#endif
