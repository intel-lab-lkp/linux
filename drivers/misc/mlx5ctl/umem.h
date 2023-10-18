/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#ifndef __MLX5CTL_UMEM_H__
#define __MLX5CTL_UMEM_H__

#include <linux/types.h>
#include <linux/mlx5/driver.h>

struct mlx5ctl_umem_db;

struct mlx5ctl_umem_db *mlx5ctl_umem_db_create(struct mlx5_core_dev *mdev, u32 uctx_uid);
void mlx5ctl_umem_db_destroy(struct mlx5ctl_umem_db *umem_db);
int mlx5ctl_umem_reg(struct mlx5ctl_umem_db *umem_db, unsigned long addr, size_t size);
int mlx5ctl_umem_unreg(struct mlx5ctl_umem_db *umem_db, u32 umem_id);

#endif /* __MLX5CTL_UMEM_H__ */
