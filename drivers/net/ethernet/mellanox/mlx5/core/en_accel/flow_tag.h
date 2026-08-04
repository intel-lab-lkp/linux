/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/* Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#ifndef __MLX5E_FLOW_TAG_H__
#define __MLX5E_FLOW_TAG_H__

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
 *              4 = PSP decap (HW decrypted & decapsulated)
 *              5-7 = reserved
 *   [20:17] = PSP version (4 bits, valid when proto == PSP decap)
 *   16      = reserved
 *   [15:0]  = used by other subsystems (e.g. TC).
 */
#define MLX5E_ACCEL_FLOW_TAG_PROTO_MASK		GENMASK(23, 21)
#define MLX5E_ACCEL_FLOW_TAG_PROTO_NONE		(0 << 21)
#define MLX5E_ACCEL_FLOW_TAG_PROTO_IPSEC	(1 << 21)
#define MLX5E_ACCEL_FLOW_TAG_PROTO_MACSEC	(2 << 21)
#define MLX5E_ACCEL_FLOW_TAG_PROTO_PSP		(3 << 21)
#define MLX5E_ACCEL_FLOW_TAG_PROTO_PSP_DECAP	(4 << 21)

#define MLX5E_ACCEL_FLOW_TAG_PSP_VER_SHIFT	17
#define MLX5E_ACCEL_FLOW_TAG_PSP_VER_MASK	(0xF << MLX5E_ACCEL_FLOW_TAG_PSP_VER_SHIFT)

static inline u32 mlx5e_accel_flow_tag(struct mlx5_cqe64 *cqe)
{
	return be32_to_cpu(cqe->sop_drop_qpn) & 0xFFFFFF;
}

static inline u32 mlx5e_accel_flow_tag_proto(struct mlx5_cqe64 *cqe)
{
	return mlx5e_accel_flow_tag(cqe) & MLX5E_ACCEL_FLOW_TAG_PROTO_MASK;
}

#endif /* __MLX5E_FLOW_TAG_H__ */
