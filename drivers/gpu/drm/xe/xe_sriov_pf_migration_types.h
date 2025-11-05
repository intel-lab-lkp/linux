/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SRIOV_PF_MIGRATION_TYPES_H_
#define _XE_SRIOV_PF_MIGRATION_TYPES_H_

#include <linux/types.h>
#include <linux/mutex_types.h>
#include <linux/wait.h>

/**
 * struct xe_sriov_pf_migration - Xe device level VF migration data
 */
struct xe_sriov_pf_migration {
	/** @supported: indicates whether VF migration feature is supported */
	bool supported;
};

/**
 * struct xe_sriov_migration_state - Per VF device-level migration related data
 */
struct xe_sriov_migration_state {
	/** @wq: waitqueue used to avoid busy-waiting for snapshot production/consumption */
	wait_queue_head_t wq;
	/** @lock: Mutex protecting the migration data */
	struct mutex lock;
	/** @pending: currently processed data packet of VF resource */
	struct xe_sriov_packet *pending;
	/** @trailer: data packet used to indicate the end of stream */
	struct xe_sriov_packet *trailer;
	/** @descriptor: data packet containing the metadata describing the device */
	struct xe_sriov_packet *descriptor;
};

/**
 * struct xe_sriov_packet - Xe SR-IOV VF migration data packet
 */
struct xe_sriov_packet {
	/** @xe: Xe device */
	struct xe_device *xe;
	/** @vaddr: CPU pointer to payload data */
	void *vaddr;
	/** @remaining: payload data remaining */
	size_t remaining;
	/** @hdr_remaining: header data remaining */
	size_t hdr_remaining;
	union {
		/** @bo: Buffer object with migration data */
		struct xe_bo *bo;
		/** @buff: Buffer with migration data */
		void *buff;
	};
	__struct_group(xe_sriov_pf_migration_hdr, hdr, __packed,
		/** @hdr.version: migration data protocol version */
		u8 version;
		/** @hdr.type: migration data type */
		u8 type;
		/** @hdr.tile: migration data tile id */
		u8 tile;
		/** @hdr.gt: migration data gt id */
		u8 gt;
		/** @hdr.flags: migration data flags */
		u32 flags;
		/** @hdr.offset: offset into the resource;
		 * used when multiple packets of given type are used for migration
		 */
		u64 offset;
		/** @hdr.size: migration data size  */
		u64 size;
	);
};

#endif
