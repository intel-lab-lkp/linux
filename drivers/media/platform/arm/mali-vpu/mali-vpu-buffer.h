/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_BUFFER_H__
#define __MALI_VPU_BUFFER_H__

#include <media/v4l2-mem2mem.h>

#include "mali-vpu-mmu.h"

#define MALI_VPU_BUFFER_MAX_PLANES 3

#define MALI_VPU_BUFFER_EOS                  0x00000001
#define MALI_VPU_BUFFER_CORRUPT              0x00000004
#define MALI_VPU_BUFFER_REJECTED             0x00000008

/**
 * enum mali_vpu_buffer_attributes - Buffer submission state.
 * @MALI_VPU_BUF_ATTR_NONE: Buffer is neither deferred nor queued for firmware
 *                          processing.
 * @MALI_VPU_BUF_ATTR_DEFERRED: Buffer is held by driver for later submission
 *                              to firmware.
 * @MALI_VPU_BUF_ATTR_QUEUED: Buffer has been submitted to firmware and is
 *                            awaiting its return.
 */
enum mali_vpu_buffer_attributes {
	MALI_VPU_BUF_ATTR_NONE,
	MALI_VPU_BUF_ATTR_DEFERRED,
	MALI_VPU_BUF_ATTR_QUEUED
};

/**
 * struct mali_vpu_buffer_plane - Plane information.
 * @pages: MMU pages object.
 * @payload_size: Number of bytes written to this plane. For a frame buffer this
 *                value should always match the size of the plane.
 * @offset: Byte offset from beginning of buffer to first bitstream data.
 * @stride: Bytes per line for the plane.
 */
struct mali_vpu_buffer_plane {
	struct mali_vpu_mmu_pages *pages;
	u32 payload_size;
	u32 offset;
	u32 stride;
};

struct mali_vpu_format_desc;

/**
 * struct mali_vpu_buffer - Buffer descriptor.
 * @m2m_buf: Embedded v4l2-mem2mem buffer.
 * @dev: Pointer to device.
 * @mmu: Pointer to MMU.
 * @nplanes: Number of planes.
 * @type: Type of the buffer (output/capture).
 * @user_data: Userdata(timestamp) to be copied to capture buffer.
 * @flags: Buffer Flags.
 * @width: Frame width in pixels.
 * @height: Frame height in pixels.
 * @fw_buffer_id: Non-zero identifier while registered for firmware submission.
 * @format: Buffer format.
 * @planes: Array of planes.
 * @attr: Firmware Queuing Status Attribute.
 */
struct mali_vpu_buffer {
	struct v4l2_m2m_buffer m2m_buf;
	struct device *dev;
	struct mali_vpu_mmu *mmu;
	u32 nplanes;
	u32 type;
	u64 user_data;
	u32 flags;
	u32 width;
	u32 height;
	u32 fw_buffer_id;
	const struct mali_vpu_format_desc *format;
	struct mali_vpu_buffer_plane planes[MALI_VPU_BUFFER_MAX_PLANES];
	enum mali_vpu_buffer_attributes attr;
};

static inline struct mali_vpu_buffer *to_mali_vpu_buffer(struct vb2_v4l2_buffer *v4l2_buf)
{
	struct v4l2_m2m_buffer *m2m_buf = container_of(v4l2_buf, struct v4l2_m2m_buffer, vb);

	return container_of(m2m_buf, struct mali_vpu_buffer, m2m_buf);
}

int mali_vpu_vb2_buffer_init(struct vb2_buffer *vb2_buf);
int mali_vpu_vb2_buffer_prepare(struct vb2_buffer *vb2_buf);
int mali_vpu_vb2_buffer_out_validate(struct vb2_buffer *vb2_buf);
void mali_vpu_vb2_buffer_cleanup(struct vb2_buffer *vb2_buf);

int mali_vpu_buffer_map(struct mali_vpu_buffer *vpu_buf, mali_vpu_mmu_va begin,
			mali_vpu_mmu_va end);
void mali_vpu_buffer_unmap(struct mali_vpu_buffer *vpu_buf);
int mali_vpu_buffer_sync(struct mali_vpu_buffer *vpu_buf, enum dma_data_direction dir);
bool mali_vpu_is_buffer_mapped(struct mali_vpu_buffer *vpu_buf);
mali_vpu_mmu_va mali_vpu_buffer_plane_va(struct mali_vpu_buffer *vpu_buf, u32 plane);

int mali_vpu_buffer_update_info_from_v4l2(struct vb2_buffer *vb2_buf);
int mali_vpu_buffer_set_plane_payload(struct mali_vpu_buffer *vpu_buf, u32 plane,
				      u32 bytesused, u32 offset);
int mali_vpu_buffer_set_frame_layout(struct mali_vpu_buffer *vpu_buf);
size_t mali_vpu_buffer_plane_size(struct mali_vpu_buffer *vpu_buf, u32 plane);
enum vb2_buffer_state mali_vpu_buffer_update_info_to_v4l2(struct mali_vpu_buffer *vpu_buf);

#endif /* __MALI_VPU_BUFFER_H__ */
