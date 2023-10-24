/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. */
#ifndef __MLX5E_NVMEOTCP_UTILS_H__
#define __MLX5E_NVMEOTCP_UTILS_H__

#include "en.h"

#define MLX5E_NVMEOTCP_FETCH_KLM_WQE(sq, pi) \
	((struct mlx5e_umr_wqe *)\
	 mlx5e_fetch_wqe(&(sq)->wq, pi, sizeof(struct mlx5e_umr_wqe)))

#define MLX5_CTRL_SEGMENT_OPC_MOD_UMR_NVMEOTCP_TIR_PROGRESS_PARAMS 0x4

#define MLX5_CTRL_SEGMENT_OPC_MOD_UMR_TIR_PARAMS 0x2
#define MLX5_CTRL_SEGMENT_OPC_MOD_UMR_UMR 0x0

enum wqe_type {
	KLM_UMR,
	BSF_KLM_UMR,
	SET_PSV_UMR,
	BSF_UMR,
	KLM_INV_UMR,
};

#endif /* __MLX5E_NVMEOTCP_UTILS_H__ */
