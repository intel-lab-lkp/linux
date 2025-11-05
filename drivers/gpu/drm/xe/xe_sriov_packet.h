/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */

#ifndef _XE_SRIOV_PACKET_H_
#define _XE_SRIOV_PACKET_H_

#include <linux/types.h>

struct xe_device;

enum xe_sriov_packet_type {
	/* Skipping 0 to catch uninitialized data */
	XE_SRIOV_PACKET_TYPE_DESCRIPTOR = 1,
	XE_SRIOV_PACKET_TYPE_TRAILER,
	XE_SRIOV_PACKET_TYPE_GGTT,
	XE_SRIOV_PACKET_TYPE_MMIO,
	XE_SRIOV_PACKET_TYPE_GUC,
	XE_SRIOV_PACKET_TYPE_VRAM,
};

struct xe_sriov_packet *xe_sriov_packet_alloc(struct xe_device *xe);
void xe_sriov_packet_free(struct xe_sriov_packet *data);

int xe_sriov_packet_init(struct xe_sriov_packet *data, u8 tile_id, u8 gt_id,
			 enum xe_sriov_packet_type, loff_t offset, size_t size);
int xe_sriov_packet_init_from_hdr(struct xe_sriov_packet *data);

ssize_t xe_sriov_packet_read_single(struct xe_device *xe, unsigned int vfid,
				    char __user *buf, size_t len);
ssize_t xe_sriov_packet_write_single(struct xe_device *xe, unsigned int vfid,
				     const char __user *buf, size_t len);
int xe_sriov_packet_save_init(struct xe_device *xe, unsigned int vfid);
int xe_sriov_packet_process_descriptor(struct xe_device *xe, unsigned int vfid,
				       struct xe_sriov_packet *data);

#endif
