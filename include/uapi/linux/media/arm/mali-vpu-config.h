/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Arm Mali-VPU Driver - Userspace API
 *
 * (C) COPYRIGHT 2026 ARM Limited.
 */

#ifndef __UAPI_MALI_VPU_CONFIG_H__
#define __UAPI_MALI_VPU_CONFIG_H__

#include <linux/v4l2-controls.h>

/* Mali VPU specific V4L2 driver controls */

/* Encoded stream NALU encapsulation mode. */
#define V4L2_CID_MALI_VPU_NALU_FORMAT		(V4L2_CID_USER_MALI_VPU_BASE + 0x00)

/**
 * enum mali_vpu_nalu_format - Encoded stream NALU OUTPUT format.
 *
 * @MALI_VPU_NALU_FORMAT_START_CODES: Each NAL unit is prefixed by an Annex B start code.
 * @MALI_VPU_NALU_FORMAT_ONE_NALU_PER_BUFFER: Exactly one NALU in each output buffer.
 * @MALI_VPU_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD: Length-prefixed NALU with 1-byte size field.
 * @MALI_VPU_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD: Length-prefixed NALU with 2-byte size field.
 * @MALI_VPU_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD: Length-prefixed NALU with 4-byte size field.
 *
 * Options for V4L2_CID_MALI_VPU_NALU_FORMAT control, which determines how the firmware
 * encapsulates NALUs in the output stream buffers H.264.
 */
enum mali_vpu_nalu_format {
	MALI_VPU_NALU_FORMAT_START_CODES		= 0,
	MALI_VPU_NALU_FORMAT_ONE_NALU_PER_BUFFER	= 1,
	MALI_VPU_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD	= 2,
	MALI_VPU_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD	= 3,
	MALI_VPU_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD	= 4,
};

#define V4L2_CID_MALI_VPU_STREAM_ESCAPING	(V4L2_CID_USER_MALI_VPU_BASE + 0x01)
#define V4L2_CID_MALI_VPU_IGNORE_STREAM_HEADERS	(V4L2_CID_USER_MALI_VPU_BASE + 0x02)
#define V4L2_CID_MALI_VPU_FRAME_REORDERING	(V4L2_CID_USER_MALI_VPU_BASE + 0x03)
/* Suggested firmware intermediate buffer size limit. */
#define V4L2_CID_MALI_VPU_INTBUF_SIZE		(V4L2_CID_USER_MALI_VPU_BASE + 0x04)

/* Enable firmware protocol to send debug prints to host. */
#define V4L2_CID_MALI_VPU_HOST_PRINT_ENABLE	(V4L2_CID_USER_MALI_VPU_BASE + 0x05)

#endif /* __UAPI_MALI_VPU_CONFIG_H__ */
