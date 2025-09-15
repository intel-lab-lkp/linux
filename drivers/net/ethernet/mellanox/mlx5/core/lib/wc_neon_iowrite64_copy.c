// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include "lib/wc_neon_iowrite64_copy.h"

void mlx5_wc_neon_iowrite64_copy(void __iomem *to, const void *from)
{
	asm volatile
	("ld1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%0]\n\t"
	"st1 {v0.16b, v1.16b, v2.16b, v3.16b}, [%1]"
	:
	: "r"(from), "r"(to)
	: "memory", "v0", "v1", "v2", "v3");
}
