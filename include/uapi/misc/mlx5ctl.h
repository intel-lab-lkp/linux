/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB WITH Linux-syscall-note */
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
	__u32 reserved2[4];
};

struct mlx5ctl_cmdrpc {
	__aligned_u64 in; /* RPC inbox buffer user address */
	__aligned_u64 out; /* RPC outbox buffer user address */
	__u32 inlen; /* inbox buffer length */
	__u32 outlen; /* outbox buffer length */
	__aligned_u64 flags;
};

struct mlx5ctl_umem_reg {
	__aligned_u64 addr; /* user address */
	__aligned_u64 len; /* user buffer length */
	__aligned_u64 flags;
	__u32 umem_id; /* returned device's umem ID */
	__u32 reserved[7];
};

#define MLX5CTL_MAX_RPC_SIZE 8192

#define MLX5CTL_IOCTL_MAGIC 0x5c

#define MLX5CTL_IOCTL_INFO \
	_IOR(MLX5CTL_IOCTL_MAGIC, 0x0, struct mlx5ctl_info)

#define MLX5CTL_IOCTL_CMDRPC \
	_IOWR(MLX5CTL_IOCTL_MAGIC, 0x1, struct mlx5ctl_cmdrpc)

#define MLX5CTL_IOCTL_UMEM_REG \
	_IOWR(MLX5CTL_IOCTL_MAGIC, 0x2, struct mlx5ctl_umem_reg)

#define MLX5CTL_IOCTL_UMEM_UNREG \
	_IOWR(MLX5CTL_IOCTL_MAGIC, 0x3, unsigned long)

#endif /* __MLX5CTL_IOCTL_H__ */
