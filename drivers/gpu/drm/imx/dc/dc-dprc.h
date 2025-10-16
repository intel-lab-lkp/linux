/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2025 NXP
 */

#ifndef __DC_DPRC_H__
#define __DC_DPRC_H__

#include <linux/device.h>
#include <linux/types.h>

#include <drm/drm_fourcc.h>

struct dc_dprc;

void dc_dprc_configure(struct dc_dprc *dprc, unsigned int stream_id,
		       unsigned int width, unsigned int height,
		       unsigned int stride,
		       const struct drm_format_info *format,
		       dma_addr_t baddr, bool start);

void dc_dprc_disable_repeat_en(struct dc_dprc *dprc);

void dc_dprc_disable(struct dc_dprc *dprc);

void dc_dprc_disable_at_boot(struct dc_dprc *dprc);

bool dc_dprc_rtram_width_supported(struct dc_dprc *dprc, unsigned int width);

bool dc_dprc_stride_supported(struct dc_dprc *dprc,
			      unsigned int stride, unsigned int width,
			      const struct drm_format_info *format,
			      dma_addr_t baddr);

#endif
