/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. */
#ifndef __MLX5E_NVMEOTCP_UTILS_H__
#define __MLX5E_NVMEOTCP_UTILS_H__

#include "en.h"
#include "en_accel/nvmeotcp.h"
#include "en_accel/common_utils.h"

enum {
	MLX5E_NVMEOTCP_PROGRESS_PARAMS_PDU_TRACKER_STATE_START     = 0,
	MLX5E_NVMEOTCP_PROGRESS_PARAMS_PDU_TRACKER_STATE_TRACKING  = 1,
	MLX5E_NVMEOTCP_PROGRESS_PARAMS_PDU_TRACKER_STATE_SEARCHING = 2,
};

struct mlx5_seg_nvmeotcp_progress_params {
	__be32 tir_num;
	u8     ctx[MLX5_ST_SZ_BYTES(nvmeotcp_progress_params)];
};

struct mlx5e_set_nvmeotcp_progress_params_wqe {
	struct mlx5_wqe_ctrl_seg            ctrl;
	struct mlx5_seg_nvmeotcp_progress_params params;
};

/* macros for wqe handling */
#define MLX5E_NVMEOTCP_PROGRESS_PARAMS_WQE_SZ \
	(sizeof(struct mlx5e_set_nvmeotcp_progress_params_wqe))

#define MLX5E_NVMEOTCP_PROGRESS_PARAMS_WQEBBS \
	(DIV_ROUND_UP(MLX5E_NVMEOTCP_PROGRESS_PARAMS_WQE_SZ, MLX5_SEND_WQE_BB))

#define MLX5E_NVMEOTCP_FETCH_PROGRESS_PARAMS_WQE(sq, pi) \
	((struct mlx5e_set_nvmeotcp_progress_params_wqe *)\
	 mlx5e_fetch_wqe(&(sq)->wq, pi, sizeof(struct mlx5e_set_nvmeotcp_progress_params_wqe)))

#define MLX5E_NVMEOTCP_FETCH_KLM_WQE(sq, pi) \
	((struct mlx5e_umr_wqe *)\
	 mlx5e_fetch_wqe(&(sq)->wq, pi, sizeof(struct mlx5e_umr_wqe)))

#define MLX5_CTRL_SEGMENT_OPC_MOD_UMR_NVMEOTCP_TIR_PROGRESS_PARAMS 0x4

#define MLX5_CTRL_SEGMENT_OPC_MOD_UMR_TIR_PARAMS 0x2
#define MLX5_CTRL_SEGMENT_OPC_MOD_UMR_UMR 0x0

#define PROGRESS_PARAMS_DS_CNT \
	DIV_ROUND_UP(MLX5E_NVMEOTCP_PROGRESS_PARAMS_WQE_SZ, MLX5_SEND_WQE_DS)

enum wqe_type {
	KLM_UMR,
	BSF_KLM_UMR,
	SET_PSV_UMR,
	BSF_UMR,
	KLM_INV_UMR,
};

void
build_nvmeotcp_progress_params(struct mlx5e_nvmeotcp_queue *queue,
			       struct mlx5e_set_nvmeotcp_progress_params_wqe *wqe, u32 seq);

void
build_nvmeotcp_static_params(struct mlx5e_nvmeotcp_queue *queue,
			     struct mlx5e_set_transport_static_params_wqe *wqe,
			     u32 resync_seq, bool crc_rx);

#endif /* __MLX5E_NVMEOTCP_UTILS_H__ */
