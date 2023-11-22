// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
// Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.

#include "en_accel/nvmeotcp.h"

struct ulp_ddp_counter_map {
	size_t eth_offset;
	size_t mlx_offset;
};

#define DECLARE_ULP_SW_STAT(fld) \
	{ offsetof(struct ulp_ddp_stats, fld), \
	  offsetof(struct mlx5e_nvmeotcp_sw_stats, fld) }

#define DECLARE_ULP_RQ_STAT(fld) \
	{ offsetof(struct ulp_ddp_stats, rx_ ## fld), \
	  offsetof(struct mlx5e_rq_stats, fld) }

#define READ_CTR_ATOMIC64(ptr, dsc, i) \
	atomic64_read((atomic64_t *)((char *)(ptr) + (dsc)[i].mlx_offset))

#define READ_CTR(ptr, desc, i) \
	(*((u64 *)((char *)(ptr) + (desc)[i].mlx_offset)))

#define SET_ULP_STAT(ptr, desc, i, val) \
	(*(u64 *)((char *)(ptr) + (desc)[i].eth_offset) = (val))

/* Global counters */
static const struct ulp_ddp_counter_map sw_stats_desc[] = {
	DECLARE_ULP_SW_STAT(rx_nvmeotcp_sk_add),
	DECLARE_ULP_SW_STAT(rx_nvmeotcp_sk_del),
	DECLARE_ULP_SW_STAT(rx_nvmeotcp_ddp_setup),
	DECLARE_ULP_SW_STAT(rx_nvmeotcp_ddp_setup_fail),
	DECLARE_ULP_SW_STAT(rx_nvmeotcp_ddp_teardown),
};

/* Per-rx-queue counters */
static const struct ulp_ddp_counter_map rq_stats_desc[] = {
	DECLARE_ULP_RQ_STAT(nvmeotcp_drop),
	DECLARE_ULP_RQ_STAT(nvmeotcp_resync),
	DECLARE_ULP_RQ_STAT(nvmeotcp_packets),
	DECLARE_ULP_RQ_STAT(nvmeotcp_bytes),
};

int mlx5e_nvmeotcp_get_stats(struct mlx5e_priv *priv, struct ulp_ddp_stats *stats)
{
	unsigned int i, ch, n = 0;

	if (!priv->nvmeotcp)
		return 0;

	for (i = 0; i < ARRAY_SIZE(sw_stats_desc); i++, n++)
		SET_ULP_STAT(stats, sw_stats_desc, i,
			     READ_CTR_ATOMIC64(&priv->nvmeotcp->sw_stats, sw_stats_desc, i));

	for (i = 0; i < ARRAY_SIZE(rq_stats_desc); i++, n++) {
		u64 sum = 0;

		for (ch = 0; ch < priv->stats_nch; ch++)
			sum += READ_CTR(&priv->channel_stats[ch]->rq, rq_stats_desc, i);

		SET_ULP_STAT(stats, rq_stats_desc, i, sum);
	}

	return n;
}
