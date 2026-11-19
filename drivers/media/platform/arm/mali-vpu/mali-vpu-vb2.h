/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_VB2_H__
#define __MALI_VPU_VB2_H__

struct vb2_queue;

int mali_vpu_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq);

#endif /* __MALI_VPU_VB2_H__ */
