// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Icenowy Zheng <uwu@icenowy.me>
 */

#include <linux/regmap.h>

#include "vs_bridge_regs.h"
#include "vs_dc.h"
#include "vs_dc_top_regs.h"
#include "vs_plane.h"
#include "vs_primary_plane_regs.h"

static void vs_dc8200_bridge_enable(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_CONFIG(output),
			VSDC_DISP_PANEL_CONFIG_RUNNING);
	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_START,
			  VSDC_DISP_PANEL_START_MULTI_DISP_SYNC);
	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_START,
			VSDC_DISP_PANEL_START_RUNNING(output));

	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_CONFIG_EX(output),
			VSDC_DISP_PANEL_CONFIG_EX_COMMIT);
}

static void vs_dc8200_bridge_disable(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_CONFIG(output),
			  VSDC_DISP_PANEL_CONFIG_RUNNING);
	regmap_clear_bits(dc->regs, VSDC_DISP_PANEL_START,
			  VSDC_DISP_PANEL_START_MULTI_DISP_SYNC |
			  VSDC_DISP_PANEL_START_RUNNING(output));

	regmap_set_bits(dc->regs, VSDC_DISP_PANEL_CONFIG_EX(output),
			VSDC_DISP_PANEL_CONFIG_EX_COMMIT);
}

static void vs_dc8200_enable_vblank(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_TOP_IRQ_EN,
			VSDC_TOP_IRQ_VSYNC(output));
}

static void vs_dc8200_disable_vblank(struct vs_dc *dc, unsigned int output)
{
	regmap_clear_bits(dc->regs, VSDC_TOP_IRQ_EN,
			  VSDC_TOP_IRQ_VSYNC(output));
}

static void vs_dc8200_plane_commit(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG_EX(output),
			VSDC_FB_CONFIG_EX_COMMIT);
}

static void vs_dc8200_plane_enable_ex(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG_EX(output),
			VSDC_FB_CONFIG_EX_FB_EN);
	regmap_update_bits(dc->regs, VSDC_FB_CONFIG_EX(output),
			   VSDC_FB_CONFIG_EX_DISPLAY_ID_MASK,
			   VSDC_FB_CONFIG_EX_DISPLAY_ID(output));

	vs_dc8200_plane_commit(dc, output);
}

static void vs_dc8200_plane_disable_ex(struct vs_dc *dc, unsigned int output)
{
	regmap_set_bits(dc->regs, VSDC_FB_CONFIG_EX(output),
			VSDC_FB_CONFIG_EX_FB_EN);

	vs_dc8200_plane_commit(dc, output);
}

static void vs_dc8200_plane_update_ex(struct vs_dc *dc, unsigned int output,
				       struct drm_plane_state *state)
{
	regmap_write(dc->regs, VSDC_FB_TOP_LEFT(output),
		     VSDC_MAKE_PLANE_POS(state->crtc_x, state->crtc_y));
	regmap_write(dc->regs, VSDC_FB_BOTTOM_RIGHT(output),
		     VSDC_MAKE_PLANE_POS(state->crtc_x + state->crtc_w,
					 state->crtc_y + state->crtc_h));
	regmap_write(dc->regs, VSDC_FB_BLEND_CONFIG(output),
		     VSDC_FB_BLEND_CONFIG_BLEND_DISABLE);

	vs_dc8200_plane_commit(dc, output);
}

static u32 vs_dc8200_irq_handler(struct vs_dc *dc)
{
	u32 irqs;

	regmap_read(dc->regs, VSDC_TOP_IRQ_ACK, &irqs);
	return irqs;
}

const struct vs_dc_funcs vs_dc8200_funcs = {
	.bridge_enable		= vs_dc8200_bridge_enable,
	.bridge_disable		= vs_dc8200_bridge_disable,
	.enable_vblank		= vs_dc8200_enable_vblank,
	.disable_vblank		= vs_dc8200_disable_vblank,
	.plane_enable_ex	= vs_dc8200_plane_enable_ex,
	.plane_disable_ex	= vs_dc8200_plane_disable_ex,
	.plane_update_ex	= vs_dc8200_plane_update_ex,
	.irq_handler		= vs_dc8200_irq_handler,
};
