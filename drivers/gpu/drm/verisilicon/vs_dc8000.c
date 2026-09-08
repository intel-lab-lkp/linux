// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Joey Lu <yclu4@nuvoton.com>
 */

#include <linux/regmap.h>

#include <drm/drm_print.h>

#include "vs_crtc_regs.h"
#include "vs_dc.h"
#include "vs_drm.h"
#include "vs_primary_plane_regs.h"

static void vs_dc8000_panel_enable_ex(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG(output),
			VSDC_FB_CONFIG_RESET);
}

static void vs_dc8000_panel_disable_ex(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_FB_CONFIG(output),
			  VSDC_FB_CONFIG_RESET);
}

static void vs_dc8000_crtc_begin(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG(output),
			VSDC_FB_CONFIG_VALID);
}

static void vs_dc8000_crtc_flush(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_FB_CONFIG(output),
			  VSDC_FB_CONFIG_VALID);
}

static void vs_dc8000_crtc_enable_ex(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG(output),
			VSDC_FB_CONFIG_ENABLE);
}

static void vs_dc8000_crtc_disable_ex(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_FB_CONFIG(output),
			  VSDC_FB_CONFIG_ENABLE);
}

static void vs_dc8000_enable_vblank(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_DISP_IRQ_EN,
			VSDC_DISP_IRQ_VSYNC(output));
}

static void vs_dc8000_disable_vblank(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_DISP_IRQ_EN,
			  VSDC_DISP_IRQ_VSYNC(output));
}

static u32 vs_dc8000_irq_ack(struct vs_dc *dc)
{
	u32 hw_irqs, unified = 0, known = 0;
	unsigned int i;

	regmap_read(dc->regs, VSDC_DISP_IRQ_STA, &hw_irqs);

	for (i = 0; i < VSDC_MAX_OUTPUTS; i++) {
		known |= VSDC_DISP_IRQ_VSYNC(i);
		if (hw_irqs & VSDC_DISP_IRQ_VSYNC(i))
			unified |= VSDC_IRQ_VSYNC(i);
	}

	drm_WARN_ONCE(&dc->drm_dev->base, hw_irqs & ~known,
		      "Unknown hardware IRQ bits: %#x\n", hw_irqs & ~known);

	return unified;
}

const struct vs_dc_funcs vs_dc8000_funcs = {
	.panel_enable_ex	= vs_dc8000_panel_enable_ex,
	.panel_disable_ex	= vs_dc8000_panel_disable_ex,
	.crtc_begin		= vs_dc8000_crtc_begin,
	.crtc_flush		= vs_dc8000_crtc_flush,
	.crtc_enable_ex		= vs_dc8000_crtc_enable_ex,
	.crtc_disable_ex	= vs_dc8000_crtc_disable_ex,
	.enable_vblank		= vs_dc8000_enable_vblank,
	.disable_vblank		= vs_dc8000_disable_vblank,
	.irq_ack		= vs_dc8000_irq_ack,
};
