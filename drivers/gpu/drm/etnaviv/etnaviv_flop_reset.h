// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Etnaviv Project
 */


#ifndef etnaviv_flop_reset_h
#define etnaviv_flop_reset_h

#include <linux/types.h>

struct etnaviv_chip_identity;
struct etnaviv_drm_private;
struct etnaviv_gpu;

bool
etnaviv_flop_reset_ppu_require(const struct etnaviv_chip_identity *chip_id);

void
etnaviv_flop_reset_ppu_init(struct etnaviv_drm_private *priv);

void
etnaviv_flop_reset_ppu_run(struct etnaviv_gpu *gpu);

#endif
