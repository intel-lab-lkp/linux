/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_BUFFER_H__
#define __MALI_VPU_BUFFER_H__

#include <media/v4l2-mem2mem.h>

#define MALI_VPU_BUFFER_MAX_PLANES 3

struct mali_vpu_buffer {
	struct v4l2_m2m_buffer m2m_buf;
};

int mali_vpu_vb2_buffer_init(struct vb2_buffer *vb2_buf);
int mali_vpu_vb2_buffer_prepare(struct vb2_buffer *vb2_buf);
int mali_vpu_vb2_buffer_out_validate(struct vb2_buffer *vb2_buf);

#endif /* __MALI_VPU_BUFFER_H__ */
