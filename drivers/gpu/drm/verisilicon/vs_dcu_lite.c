// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Joey Lu <yclu4@nuvoton.com>
 */

#include <linux/regmap.h>

#include "vs_crtc_regs.h"
#include "vs_dc.h"
#include "vs_primary_plane_regs.h"

static void vs_dcu_lite_bridge_enable(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG(output),
			VSDC_FB_CONFIG_RESET);
}

static void vs_dcu_lite_bridge_disable(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_FB_CONFIG(output),
			  VSDC_FB_CONFIG_RESET);
}

static void vs_dcu_lite_crtc_begin(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG(output),
			VSDC_FB_CONFIG_VALID);
}

static void vs_dcu_lite_crtc_flush(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_FB_CONFIG(output),
			  VSDC_FB_CONFIG_VALID);
}

static void vs_dcu_lite_crtc_enable(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG(output),
			VSDC_FB_CONFIG_ENABLE);
}

static void vs_dcu_lite_crtc_disable(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_FB_CONFIG(output),
			  VSDC_FB_CONFIG_ENABLE);
}

static void vs_dcu_lite_enable_vblank(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_DISP_IRQ_EN,
			VSDC_DISP_IRQ_VSYNC(output));
}

static void vs_dcu_lite_disable_vblank(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_DISP_IRQ_EN,
			  VSDC_DISP_IRQ_VSYNC(output));
}

static u32 vs_dcu_lite_irq_handler(struct vs_dc *dc)
{
	u32 irqs;

	regmap_read(dc->regs, VSDC_DISP_IRQ_STA, &irqs);
	return irqs;
}

const struct vs_dc_funcs vs_dcu_lite_funcs = {
	.bridge_enable		= vs_dcu_lite_bridge_enable,
	.bridge_disable		= vs_dcu_lite_bridge_disable,
	.crtc_begin		= vs_dcu_lite_crtc_begin,
	.crtc_flush		= vs_dcu_lite_crtc_flush,
	.crtc_enable		= vs_dcu_lite_crtc_enable,
	.crtc_disable		= vs_dcu_lite_crtc_disable,
	.enable_vblank		= vs_dcu_lite_enable_vblank,
	.disable_vblank		= vs_dcu_lite_disable_vblank,
	.irq_handler		= vs_dcu_lite_irq_handler,
};
