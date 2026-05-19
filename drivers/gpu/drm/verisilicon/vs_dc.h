/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Icenowy Zheng <uwu@icenowy.me>
 *
 * Based on vs_dc_hw.h, which is:
 *   Copyright (C) 2023 VeriSilicon Holdings Co., Ltd.
 */

#ifndef _VS_DC_H_
#define _VS_DC_H_

#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <drm/drm_device.h>
#include <drm/drm_plane.h>

#include "vs_hwdb.h"

#define VSDC_MAX_OUTPUTS 2
#define VSDC_RESET_COUNT 3

struct vs_drm_dev;
struct vs_crtc;
struct vs_dc;

struct vs_dc_funcs {
	/* Bridge: atomic_enable, atomic_disable */
	void (*bridge_enable)(struct vs_dc *dc, unsigned int output);
	void (*bridge_disable)(struct vs_dc *dc, unsigned int output);

	/* CRTC: atomic_begin, atomic_flush */
	void (*crtc_begin)(struct vs_dc *dc, unsigned int output);
	void (*crtc_flush)(struct vs_dc *dc, unsigned int output);

	/* CRTC: atomic_enable, atomic_disable */
	void (*crtc_enable)(struct vs_dc *dc, unsigned int output);
	void (*crtc_disable)(struct vs_dc *dc, unsigned int output);

	/* CRTC: enable_vblank, disable_vblank */
	void (*enable_vblank)(struct vs_dc *dc, unsigned int output);
	void (*disable_vblank)(struct vs_dc *dc, unsigned int output);

	/* Primary plane: atomic_enable, atomic_disable, atomic_update */
	void (*plane_enable_ex)(struct vs_dc *dc, unsigned int output);
	void (*plane_disable_ex)(struct vs_dc *dc, unsigned int output);
	void (*plane_update_ex)(struct vs_dc *dc, unsigned int output,
				struct drm_plane_state *state);

	/* IRQ handler */
	u32 (*irq_handler)(struct vs_dc *dc);
};

struct vs_dc {
	struct regmap *regs;
	struct clk *core_clk;
	struct clk *axi_clk;
	struct clk *ahb_clk;
	struct clk *pix_clk[VSDC_MAX_OUTPUTS];
	struct reset_control_bulk_data rsts[VSDC_RESET_COUNT];

	struct vs_drm_dev *drm_dev;
	struct vs_chip_identity identity;
	const struct vs_dc_funcs *funcs;
};

extern const struct vs_dc_funcs vs_dc8200_funcs;
extern const struct vs_dc_funcs vs_dcu_lite_funcs;

#endif /* _VS_DC_H_ */
