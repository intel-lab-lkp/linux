/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#ifndef __MLX5E_FLOW_TAG_H__
#define __MLX5E_FLOW_TAG_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mlx5/device.h>

/* Unified accel flow_tag layout in CQE sop_drop_qpn [23:0]:
 *
 *   [23:21] = protocol ID (3 bits):
 *              0 = none (default)
 *              1 = IPsec
 *              2 = MACsec
 *              3 = PSP (HW decrypted, PSP header present)
 *              4-7 = reserved
 *   [20:16] = reserved
 *   [15:0]  = used by other subsystems (e.g. TC).
 */
#define MLX5E_ACCEL_FLOW_TAG_MASK		GENMASK(23, 0)
#define MLX5E_ACCEL_FLOW_TAG_PROTO_MASK		GENMASK(23, 21)

enum {
	MLX5E_ACCEL_FLOW_TAG_PROTO_NONE		= 0,
	MLX5E_ACCEL_FLOW_TAG_PROTO_IPSEC	= 1,
	MLX5E_ACCEL_FLOW_TAG_PROTO_MACSEC	= 2,
	MLX5E_ACCEL_FLOW_TAG_PROTO_PSP		= 3,
};

static inline u32 mlx5e_accel_flow_tag(struct mlx5_cqe64 *cqe)
{
	return be32_to_cpu(cqe->sop_drop_qpn) & MLX5E_ACCEL_FLOW_TAG_MASK;
}

static inline u32 mlx5e_accel_flow_tag_proto(struct mlx5_cqe64 *cqe)
{
	return FIELD_GET(MLX5E_ACCEL_FLOW_TAG_PROTO_MASK,
			 mlx5e_accel_flow_tag(cqe));
}

#endif /* __MLX5E_FLOW_TAG_H__ */
