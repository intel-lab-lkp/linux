/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES. */
#ifndef __MLX5E_NVMEOTCP_RXTX_H__
#define __MLX5E_NVMEOTCP_RXTX_H__

#ifdef CONFIG_MLX5_EN_NVMEOTCP

#include <linux/skbuff.h>
#include "en_accel/nvmeotcp.h"

bool
mlx5e_nvmeotcp_rebuild_rx_skb(struct mlx5e_rq *rq, struct sk_buff *skb,
			      struct mlx5_cqe64 *cqe, u32 cqe_bcnt);

static inline int mlx5_nvmeotcp_get_headlen(struct mlx5_cqe64 *cqe, u32 cqe_bcnt)
{
	struct mlx5e_cqe128 *cqe128;

	if (!cqe_is_nvmeotcp_zc(cqe))
		return cqe_bcnt;

	cqe128 = container_of(cqe, struct mlx5e_cqe128, cqe64);
	return be16_to_cpu(cqe128->hlen);
}

#else

static inline bool
mlx5e_nvmeotcp_rebuild_rx_skb(struct mlx5e_rq *rq, struct sk_buff *skb,
			      struct mlx5_cqe64 *cqe, u32 cqe_bcnt)
{ return true; }

static inline int mlx5_nvmeotcp_get_headlen(struct mlx5_cqe64 *cqe, u32 cqe_bcnt)
{ return cqe_bcnt; }

#endif /* CONFIG_MLX5_EN_NVMEOTCP */
#endif /* __MLX5E_NVMEOTCP_RXTX_H__ */
