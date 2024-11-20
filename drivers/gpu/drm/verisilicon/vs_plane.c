// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) VeriSilicon Holdings Co., Ltd.
 */
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_plane_helper.h>

#include "vs_plane.h"
#include "vs_drv.h"
#include "vs_crtc.h"

static inline struct vs_plane_state *
to_vs_plane_state(struct drm_plane_state *state)
{
	return container_of(state, struct vs_plane_state, base);
}

static inline struct vs_plane *to_vs_plane(struct drm_plane *plane)
{
	return container_of(plane, struct vs_plane, base);
}

static void vs_plane_atomic_destroy_state(struct drm_plane *plane,
					  struct drm_plane_state *state)
{
	struct vs_plane_state *vs_plane_state = to_vs_plane_state(state);

	__drm_atomic_helper_plane_destroy_state(state);
	kfree(vs_plane_state);
}

static void vs_plane_reset(struct drm_plane *plane)
{
	struct vs_plane_state *state;

	if (plane->state)
		vs_plane_atomic_destroy_state(plane, plane->state);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return;

	__drm_atomic_helper_plane_reset(plane, &state->base);
}

static struct drm_plane_state *
vs_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct vs_plane_state *state;

	if (WARN_ON(!plane->state))
		return NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &state->base);

	return &state->base;
}

static bool vs_format_mod_supported(struct drm_plane *plane,
				    u32 format,
				    u64 modifier)
{
	int i;

	/* We always have to allow these modifiers:
	 * 1. Core DRM checks for LINEAR support if userspace does not provide modifiers.
	 * 2. Not passing any modifiers is the same as explicitly passing INVALID.
	 */
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return true;

	/* Check that the modifier is on the list of the plane's supported modifiers. */
	for (i = 0; i < plane->modifier_count; i++) {
		if (modifier == plane->modifiers[i])
			break;
	}

	if (i == plane->modifier_count)
		return false;

	return true;
}

static const struct drm_plane_funcs vs_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= vs_plane_reset,
	.atomic_duplicate_state = vs_plane_atomic_duplicate_state,
	.atomic_destroy_state	= vs_plane_atomic_destroy_state,
	.format_mod_supported	= vs_format_mod_supported,
};

static unsigned char vs_get_plane_number(struct drm_framebuffer *fb)
{
	const struct drm_format_info *info;

	if (!fb)
		return 0;

	info = drm_format_info(fb->format->format);
	if (!info || info->num_planes > DRM_FORMAT_MAX_PLANES)
		return 0;

	return info->num_planes;
}

static bool vs_dc_mod_supported(const struct vs_plane_info *vs_info, u64 modifier)
{
	const u64 *mods;

	if (vs_info->type == DRM_PLANE_TYPE_CURSOR)
		return 0;

	if (!vs_info->data->modifiers)
		return false;

	for (mods = vs_info->data->modifiers; *mods != DRM_FORMAT_MOD_INVALID; mods++) {
		if (*mods == modifier)
			return true;
	}

	return false;
}

static int vs_common_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct vs_drm_device *priv = to_vs_drm_private(plane->dev);
	struct vs_dc *dc = &priv->dc;
	struct drm_framebuffer *fb = new_state->fb;
	const struct vs_plane_info *plane_info;
	struct drm_crtc_state *crtc_state;

	if (!new_state->crtc || !new_state->fb)
		return 0;

	plane_info = (struct vs_plane_info *)&dc->hw.info->info[to_vs_plane(plane)->id];

	if (fb->width < plane_info->data->min_width ||
	    fb->width > plane_info->data->max_width ||
	    fb->height < plane_info->data->min_height ||
	    fb->height > plane_info->data->max_height)
		drm_err_once(plane->dev, "buffer size may not support on plane%d.\n",
			     to_vs_plane(plane)->id);

	if (!vs_dc_mod_supported(plane_info, fb->modifier)) {
		drm_err(plane->dev, "unsupported modifier on plane%d.\n", to_vs_plane(plane)->id);
		return -EINVAL;
	}

	crtc_state = drm_atomic_get_existing_crtc_state(new_state->state, new_state->crtc);
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   plane_info->data->min_scale,
						   plane_info->data->max_scale,
						   true, true);
}

static void vs_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);

	unsigned char i, num_planes, display_id, id;
	u32 format;
	bool is_yuv;
	struct vs_plane *vs_plane = to_vs_plane(plane);
	struct vs_plane_state *plane_state = to_vs_plane_state(new_state);
	struct vs_drm_device *priv = to_vs_drm_private(plane->dev);
	struct vs_dc *dc = &priv->dc;

	if (!new_state->fb || !new_state->crtc)
		return;

	drm_fb_dma_sync_non_coherent(plane->dev, old_state, new_state);

	num_planes = vs_get_plane_number(new_state->fb);

	for (i = 0; i < num_planes; i++) {
		dma_addr_t dma_addr;

		dma_addr = drm_fb_dma_get_gem_addr(new_state->fb, new_state, i);
		plane_state->dma_addr[i] = dma_addr;
	}

	display_id = to_vs_display_id(new_state->crtc);
	format = new_state->fb->format->format;
	is_yuv = new_state->fb->format->is_yuv;
	id = vs_plane->id;

	dc_plane_hw_update_format_colorspace(dc, format, new_state->color_encoding, id, is_yuv);
	if (new_state->visible)
		dc_plane_hw_update_address(dc, id, format, plane_state->dma_addr,
					   new_state->fb, &new_state->src);
	dc_plane_hw_update_format(dc, format, new_state->color_encoding, new_state->rotation,
				  new_state->visible, new_state->zpos, id, display_id);
	dc_plane_hw_update_scale(dc, &new_state->src, &new_state->dst, id,
				 display_id, new_state->rotation);
	dc_plane_hw_update_blend(dc, new_state->alpha, new_state->pixel_blend_mode,
				 id, display_id);
}

static void vs_cursor_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state,
									   plane);
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state,
									   plane);
	struct vs_drm_device *priv = to_vs_drm_private(plane->dev);
	struct vs_dc *dc = &priv->dc;
	unsigned char display_id;
	u32 crtc_w, crtc_x, crtc_y;
	s32 hotspot_x, hotspot_y;
	dma_addr_t dma_addr;

	display_id = to_vs_display_id(new_state->crtc);

	if (!new_state->fb || !new_state->crtc)
		return;

	drm_fb_dma_sync_non_coherent(new_state->fb->dev, old_state, new_state);
	dma_addr = drm_fb_dma_get_gem_addr(new_state->fb, new_state, 0);
	crtc_w = new_state->crtc_w;

	if (new_state->crtc_x > 0) {
		crtc_x = new_state->crtc_x;
		hotspot_x = 0;
	} else {
		hotspot_x = -new_state->crtc_x;
		crtc_x = 0;
	}
	if (new_state->crtc_y > 0) {
		crtc_y = new_state->crtc_y;
		hotspot_y = 0;
	} else {
		hotspot_y = -new_state->crtc_y;
		crtc_y = 0;
	}
	dc_hw_update_cursor(&dc->hw, display_id, dma_addr, crtc_w, crtc_x,
			    crtc_y, hotspot_x, hotspot_y);
}

static void vs_plane_atomic_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct vs_plane *vs_plane = to_vs_plane(plane);
	struct vs_drm_device *priv = to_vs_drm_private(plane->dev);
	struct vs_dc *dc = &priv->dc;

	dc_hw_disable_plane(dc, vs_plane->id);
}

static void vs_cursor_plane_atomic_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *old_state = drm_atomic_get_old_plane_state(state, plane);
	struct vs_drm_device *priv = to_vs_drm_private(plane->dev);
	struct vs_dc *dc = &priv->dc;
	unsigned char display_id;

	display_id = to_vs_display_id(old_state->crtc);
	dc_hw_disable_cursor(&dc->hw, display_id);
}

static const struct drm_plane_helper_funcs vs_primary_plane_helpers = {
	.atomic_check	= vs_common_plane_atomic_check,
	.atomic_update	= vs_plane_atomic_update,
	.atomic_disable = vs_plane_atomic_disable,
};

static const struct drm_plane_helper_funcs vs_overlay_plane_helpers = {
	.atomic_check	= vs_common_plane_atomic_check,
	.atomic_update	= vs_plane_atomic_update,
	.atomic_disable = vs_plane_atomic_disable,
};

static const struct drm_plane_helper_funcs vs_cursor_plane_helpers = {
	.atomic_check	= vs_common_plane_atomic_check,
	.atomic_update	= vs_cursor_plane_atomic_update,
	.atomic_disable = vs_cursor_plane_atomic_disable,
};

struct vs_plane *vs_plane_create(struct drm_device *drm_dev,
				 struct vs_plane_info *info,
				 unsigned int layer_num,
				 unsigned int possible_crtcs)
{
	struct vs_plane *plane;
	const struct vs_plane_data *data = info->data;
	int ret;

	if (!info)
		return NULL;

	plane = drmm_universal_plane_alloc(drm_dev, struct vs_plane, base,
					   possible_crtcs,
					   &vs_plane_funcs,
					   data->formats, data->num_formats,
					   data->modifiers, info->type,
					   NULL);
	if (IS_ERR(plane))
		return ERR_CAST(plane);

	if (info->type == DRM_PLANE_TYPE_PRIMARY)
		drm_plane_helper_add(&plane->base, &vs_primary_plane_helpers);
	else if (info->type == DRM_PLANE_TYPE_OVERLAY)
		drm_plane_helper_add(&plane->base, &vs_overlay_plane_helpers);
	else
		drm_plane_helper_add(&plane->base, &vs_cursor_plane_helpers);

	if (data->blend_mode) {
		ret = drm_plane_create_alpha_property(&plane->base);
		if (ret)
			return NULL;

		ret = drm_plane_create_blend_mode_property(&plane->base,
							   BIT(DRM_MODE_BLEND_PIXEL_NONE) |
							   BIT(DRM_MODE_BLEND_PREMULTI) |
							   BIT(DRM_MODE_BLEND_COVERAGE));
		if (ret)
			return NULL;
	}

	if (data->color_encoding) {
		ret = drm_plane_create_color_properties(&plane->base, data->color_encoding,
							BIT(DRM_COLOR_YCBCR_LIMITED_RANGE),
							DRM_COLOR_YCBCR_BT709,
							DRM_COLOR_YCBCR_LIMITED_RANGE);
		if (ret)
			return NULL;
	}

	if (data->rotation) {
		ret = drm_plane_create_rotation_property(&plane->base,
							 DRM_MODE_ROTATE_0,
							 data->rotation);
		if (ret)
			return NULL;
	}

	if (data->zpos != 255) {
		ret = drm_plane_create_zpos_property(&plane->base, data->zpos, 0, layer_num - 1);
		if (ret)
			return NULL;
	} else {
		ret = drm_plane_create_zpos_immutable_property(&plane->base, data->zpos);
		if (ret)
			return NULL;
	}

	return plane;
}
