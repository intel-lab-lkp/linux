/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 WITH Linux-syscall-note */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#ifndef __MLX5CTL_IOCTL_H__
#define __MLX5CTL_IOCTL_H__

struct mlx5ctl_info {
	__aligned_u64 flags;
	__u32 size;
	__u8 devname[64]; /* underlaying ConnectX device */
	__u16 uctx_uid; /* current process allocated UCTX UID */
	__u16 reserved1;
	__u32 uctx_cap; /* current process effective UCTX cap */
	__u32 dev_uctx_cap; /* device's UCTX capabilities */
	__u32 ucap; /* process user capability */
	__u32 reserved2;
};

#define MLX5CTL_IOCTL_MAGIC 0x5c

#define MLX5CTL_IOCTL_INFO \
	_IOR(MLX5CTL_IOCTL_MAGIC, 0x0, struct mlx5ctl_info)

#endif /* __MLX5CTL_IOCTL_H__ */
