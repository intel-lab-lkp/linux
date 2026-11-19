/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2026 ARM Limited.
 */

#ifndef __MALI_VPU_VIDIOC_H__
#define __MALI_VPU_VIDIOC_H__

struct v4l2_file_operations;
struct v4l2_ioctl_ops;
struct v4l2_m2m_ops;

extern const struct v4l2_file_operations mali_vpu_fops;
extern const struct v4l2_m2m_ops mali_vpu_m2m_ops;
extern const struct v4l2_ioctl_ops mali_vpu_ioctl_ops_dec;

#endif /* __MALI_VPU_VIDIOC_H__ */
