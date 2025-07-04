/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2025 NXP
 */

#ifndef __DC_PRG_H__
#define __DC_PRG_H__

#include <linux/device.h>
#include <linux/types.h>

struct dc_prg;

void dc_prg_enable(struct dc_prg *prg);

void dc_prg_disable(struct dc_prg *prg);

void dc_prg_disable_at_boot(struct dc_prg *prg);

void dc_prg_configure(struct dc_prg *prg,
		      unsigned int width, unsigned int height,
		      unsigned int stride, unsigned int bits_per_pixel,
		      dma_addr_t baddr, bool start);

void dc_prg_reg_update(struct dc_prg *prg);

void dc_prg_shadow_enable(struct dc_prg *prg);

bool dc_prg_stride_supported(struct dc_prg *prg,
			     unsigned int stride, dma_addr_t baddr);

struct dc_prg *
dc_prg_lookup_by_phandle(struct device *dev, const char *name, int index);

void dc_prg_set_dprc(struct dc_prg *prg, struct dc_dprc *dprc);

struct dc_dprc *dc_prg_get_dprc(struct dc_prg *prg);

#endif
