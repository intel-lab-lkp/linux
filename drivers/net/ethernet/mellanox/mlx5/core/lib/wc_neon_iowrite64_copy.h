/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#ifndef __MLX5_LIB_WC_NEON_H__
#define __MLX5_LIB_WC_NEON_H__

/* Executes a 64 byte copy between the two provided pointers via ARM neon
 * instruction.
 */
void mlx5_wc_neon_iowrite64_copy(void __iomem *to, const void *from);

#endif /* __MLX5_LIB_WC_NEON_H__ */
