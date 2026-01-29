// SPDX-License-Identifier: GPL-2.0+
/*
 * Nuvoton DRM driver
 *
 * Copyright (C) 2026 Nuvoton Technology Corp.
 *
 * Author: Joey Lu <a0987203069@gmail.com>
 */

#include <linux/of.h>
#include <linux/types.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_plane.h>
#include <drm/drm_print.h>

#include "ma35_drm.h"

#define ma35_layer(p) \
	container_of(p, struct ma35_layer, drm_plane)

static uint32_t ma35_layer_formats[] = {
	/* rgb32 */
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB2101010,
	/* rgb16 */
	DRM_FORMAT_XRGB4444,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_RGB565,
	/* yuv */
	DRM_FORMAT_YUYV,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV16,
	DRM_FORMAT_P010,
};

static uint32_t ma35_cursor_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static struct ma35_plane_property ma35_plane_properties[] = {
	{ /* overlay */
		.fb_addr    = { MA35_OVERLAY_ADDRESS,
						MA35_OVERLAY_UPLANAR_ADDRESS,
						MA35_OVERLAY_VPLANAR_ADDRESS },
		.fb_stride  = { MA35_OVERLAY_STRIDE,
						MA35_OVERLAY_USTRIDE,
						MA35_OVERLAY_VSTRIDE },
		.alpha      = true,
	},
	{ /* primary */
		.fb_addr    = { MA35_FRAMEBUFFER_ADDRESS,
						MA35_FRAMEBUFFER_UPLANAR_ADDRESS,
						MA35_FRAMEBUFFER_VPLANAR_ADDRESS },
		.fb_stride  = { MA35_FRAMEBUFFER_STRIDE,
						MA35_FRAMEBUFFER_USTRIDE,
						MA35_FRAMEBUFFER_VSTRIDE },
		.alpha      = false,
	},
};

static int ma35_layer_format_validate(u32 fourcc, u32 *format)
{
	switch (fourcc) {
	case DRM_FORMAT_XRGB4444:
		*format = MA35_FORMAT_X4R4G4B4;
		break;
	case DRM_FORMAT_ARGB4444:
		*format = MA35_FORMAT_A4R4G4B4;
		break;
	case DRM_FORMAT_XRGB1555:
		*format = MA35_FORMAT_X1R5G5B5;
		break;
	case DRM_FORMAT_ARGB1555:
		*format = MA35_FORMAT_A1R5G5B5;
		break;
	case DRM_FORMAT_RGB565:
		*format = MA35_FORMAT_R5G6B5;
		break;
	case DRM_FORMAT_XRGB8888:
		*format = MA35_FORMAT_X8R8G8B8;
		break;
	case DRM_FORMAT_ARGB8888:
		*format = MA35_FORMAT_A8R8G8B8;
		break;
	case DRM_FORMAT_ARGB2101010:
		*format = MA35_FORMAT_A2R10G10B10;
		break;
	case DRM_FORMAT_YUYV:
		*format = MA35_FORMAT_YUY2;
		break;
	case DRM_FORMAT_UYVY:
		*format = MA35_FORMAT_UYVY;
		break;
	case DRM_FORMAT_YVU420:
		*format = MA35_FORMAT_YV12;
		break;
	case DRM_FORMAT_NV12:
		*format = MA35_FORMAT_NV12;
		break;
	case DRM_FORMAT_NV16:
		*format = MA35_FORMAT_NV16;
		break;
	case DRM_FORMAT_P010:
		*format = MA35_FORMAT_P010;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int ma35_plane_atomic_check(struct drm_plane *drm_plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *drm_dev = drm_plane->dev;
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, drm_plane);
	struct drm_crtc *crtc = new_state->crtc;
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc_state *crtc_state;
	bool can_position;
	u32 format;

	if (!crtc)
		return 0;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	if (new_state->crtc_x < 0 || new_state->crtc_y < 0) {
		drm_err(drm_dev,
			"Negative on-CRTC positions are not supported.\n");
		return -EINVAL;
	}

	if (ma35_layer_format_validate(fb->format->format, &format) < 0) {
		drm_err(drm_dev, "Unsupported format\n");
		return -EINVAL;
	}

	can_position = (drm_plane->type != DRM_PLANE_TYPE_PRIMARY);
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						  can_position, true);
}

static int ma35_cursor_plane_atomic_check(struct drm_plane *drm_plane,
				      struct drm_atomic_state *state)
{
	struct drm_device *drm_dev = drm_plane->dev;
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, drm_plane);
	struct drm_framebuffer *fb = new_state->fb;
	struct drm_crtc *crtc = new_state->crtc;
	struct drm_crtc_state *crtc_state;

	if (!fb)
		return 0;

	if (!crtc)
		return -EINVAL;

	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	if (fb->format->format != DRM_FORMAT_XRGB8888) {
		drm_err(drm_dev, "Invalid cursor format\n");
		return -EINVAL;
	}

	if (new_state->crtc_w != MA35_CURSOR_SIZE || new_state->crtc_h != MA35_CURSOR_SIZE) {
		drm_err(drm_dev, "Unsupported cursor size: %ux%u\n",
				new_state->crtc_w, new_state->crtc_h);
		return -EINVAL;
	}

	if (new_state->hotspot_x >= 32 || new_state->hotspot_x < 0 ||
		new_state->hotspot_y >= 32 || new_state->hotspot_y < 0) {
		drm_err(drm_dev, "Invalid cursor hotspot offset\n");
		return -EINVAL;
	}

	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						  DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
						  true, true);
}

static int ma35_cursor_plane_atomic_async_check(struct drm_plane *drm_plane,
				      struct drm_atomic_state *state, bool flip)
{
	return ma35_cursor_plane_atomic_check(drm_plane, state);
}

static void ma35_overlay_position_update(struct ma35_drm *priv,
						int x, int y, uint32_t w, uint32_t h)
{
	u32 reg;
	int right, bottom;

	right = x + w;
	bottom = y + h;

	x = (x < 0) ? 0 : x;
	y = (y < 0) ? 0 : y;
	right = (right < 0) ? 0 : right;
	bottom = (bottom < 0) ? 0 : bottom;

	reg = FIELD_PREP(MA35_OVERLAY_POSITION_X_MASK, x) |
		  FIELD_PREP(MA35_OVERLAY_POSITION_Y_MASK, y);
	regmap_write(priv->regmap, MA35_OVERLAY_TL, reg);

	reg = FIELD_PREP(MA35_OVERLAY_POSITION_X_MASK, right) |
		  FIELD_PREP(MA35_OVERLAY_POSITION_Y_MASK, bottom);
	regmap_write(priv->regmap, MA35_OVERLAY_BR, reg);
}

static void ma35_plane_atomic_update(struct drm_plane *drm_plane,
					struct drm_atomic_state *state)
{
	struct ma35_layer *layer = ma35_layer(drm_plane);
	struct ma35_drm *priv = ma35_drm(drm_plane->dev);
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, drm_plane);
	struct drm_framebuffer *fb = new_state->fb;
	u32 format, reg;
	u32 *preg;

	ma35_layer_format_validate(fb->format->format, &format);

	if (drm_plane->type == DRM_PLANE_TYPE_PRIMARY) {
		reg = FIELD_PREP(MA35_PRIMARY_FORMAT_MASK, format) |
			  MA35_PRIMARY_RESET | MA35_PRIMARY_ENABLE;
		regmap_write(priv->regmap, MA35_FRAMEBUFFER_CONFIG, reg);

		reg = FIELD_PREP(MA35_LAYER_FB_HEIGHT, fb->height) |
			  FIELD_PREP(MA35_LAYER_FB_WIDTH, fb->width);
		regmap_write(priv->regmap, MA35_FRAMEBUFFER_SIZE, reg);

		/* clear value */
		regmap_write(priv->regmap, MA35_FRAMEBUFFER_CLEARVALUE, 0);
	} else if (drm_plane->type == DRM_PLANE_TYPE_OVERLAY) {
		reg = FIELD_PREP(MA35_OVERLAY_FORMAT_MASK, format) |
			  MA35_OVERLAY_ENABLE;
		regmap_write(priv->regmap, MA35_OVERLAY_CONFIG, reg);

		reg = FIELD_PREP(MA35_LAYER_FB_HEIGHT, fb->height) |
			FIELD_PREP(MA35_LAYER_FB_WIDTH, fb->width);
		regmap_write(priv->regmap, MA35_OVERLAY_SIZE, reg);

		/* can_position */
		ma35_overlay_position_update(priv, new_state->crtc_x, new_state->crtc_y,
			new_state->crtc_w, new_state->crtc_h);

		/* alpha blending */
		if (fb->format->format == DRM_FORMAT_ARGB8888) {
			if (new_state->pixel_blend_mode &
				(DRM_MODE_BLEND_PREMULTI | DRM_MODE_BLEND_COVERAGE))
				reg = MA35_BLEND_MODE_SRC_OVER;
			if (new_state->pixel_blend_mode & DRM_MODE_BLEND_COVERAGE)
				reg |= MA35_SRC_ALPHA_FACTOR_EN;
			regmap_write(priv->regmap, MA35_OVERLAY_ALPHA_BLEND_CONFIG, reg);
		} else {
			regmap_update_bits(priv->regmap, MA35_OVERLAY_ALPHA_BLEND_CONFIG,
				MA35_ALPHA_BLEND_DISABLE, MA35_ALPHA_BLEND_DISABLE);
		}

		/* clear value */
		regmap_write(priv->regmap, MA35_OVERLAY_CLEAR_VALUE, 0);
	}

	/* retrieves DMA address set by userspace */
	for (int i = 0; i < fb->format->num_planes; i++) {
		layer->fb_base[i] = drm_fb_dma_get_gem_addr(fb, new_state, i);
		preg = ma35_plane_properties[drm_plane->type].fb_addr;
		regmap_write(priv->regmap, preg[i], layer->fb_base[i]);
		preg = ma35_plane_properties[drm_plane->type].fb_stride;
		regmap_write(priv->regmap, preg[i], fb->pitches[i]);
	}
}

static void ma35_cursor_position_update(struct ma35_drm *priv, int x, int y)
{
	u32 reg;

	x = (x < 0) ? 0 : x;
	y = (y < 0) ? 0 : y;

	reg = FIELD_PREP(MA35_CURSOR_X_MASK, x) |
		  FIELD_PREP(MA35_CURSOR_Y_MASK, y);
	regmap_write(priv->regmap, MA35_CURSOR_LOCATION, reg);
}

static void ma35_cursor_plane_atomic_update(struct drm_plane *drm_plane,
					struct drm_atomic_state *state)
{
	struct ma35_layer *layer = ma35_layer(drm_plane);
	struct ma35_drm *priv = ma35_drm(drm_plane->dev);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, drm_plane);
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, drm_plane);
	struct drm_framebuffer *old_fb = old_state->fb;
	struct drm_framebuffer *new_fb = new_state->fb;
	u32 reg;

	if (!new_state->visible) {
		regmap_update_bits(priv->regmap, MA35_CURSOR_CONFIG,
			MA35_CURSOR_FORMAT_MASK, MA35_CURSOR_FORMAT_DISABLE);
		return;
	}

	/* update position */
	ma35_cursor_position_update(priv, new_state->crtc_x, new_state->crtc_y);

	/* check new_state is different from old_state for dimensions or format changed */
	if (!old_fb || old_fb != new_fb) {
		layer->fb_base[0] = drm_fb_dma_get_gem_addr(new_fb, new_state, 0);
		regmap_write(priv->regmap, MA35_CURSOR_ADDRESS, layer->fb_base[0]);
		regmap_update_bits(priv->regmap, MA35_CURSOR_CONFIG,
			MA35_CURSOR_FORMAT_MASK, MA35_CURSOR_FORMAT_A8R8G8B8);
	}

	/* update hotspot offset & format */
	if (old_state->hotspot_x != new_state->hotspot_x ||
		old_state->hotspot_y != new_state->hotspot_y) {
		reg = MA35_CURSOR_FORMAT_A8R8G8B8 |
			FIELD_PREP(MA35_CURSOR_HOTSPOT_X_MASK, new_state->hotspot_x) |
			FIELD_PREP(MA35_CURSOR_HOTSPOT_Y_MASK, new_state->hotspot_y);
		regmap_write(priv->regmap, MA35_CURSOR_CONFIG, reg);
	}
}

static void ma35_cursor_plane_atomic_async_update(struct drm_plane *drm_plane,
					struct drm_atomic_state *state)
{
	struct ma35_layer *layer = ma35_layer(drm_plane);
	struct ma35_drm *priv = ma35_drm(drm_plane->dev);
	struct drm_plane_state *old_state = drm_plane->state;
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, drm_plane);
	struct drm_framebuffer *old_fb = old_state->fb;
	struct drm_framebuffer *new_fb = new_state->fb;
	u32 reg;

	/* update the current one with the new plane state */
	old_state->crtc_x = new_state->crtc_x;
	old_state->crtc_y = new_state->crtc_y;
	old_state->crtc_h = new_state->crtc_h;
	old_state->crtc_w = new_state->crtc_w;
	old_state->src_x = new_state->src_x;
	old_state->src_y = new_state->src_y;
	old_state->src_h = new_state->src_h;
	old_state->src_w = new_state->src_w;
	/* swap current and new framebuffers */
	swap(old_fb, new_fb);

	if (!new_state->visible) {
		regmap_update_bits(priv->regmap, MA35_CURSOR_CONFIG,
			MA35_CURSOR_FORMAT_MASK, MA35_CURSOR_FORMAT_DISABLE);
		return;
	}

	/* update position */
	ma35_cursor_position_update(priv, new_state->crtc_x, new_state->crtc_y);

	/* check new_state is different from old_state for dimensions or format changed */
	if (!old_fb || old_fb != new_fb) {
		layer->fb_base[0] = drm_fb_dma_get_gem_addr(new_fb, new_state, 0);
		regmap_write(priv->regmap, MA35_CURSOR_ADDRESS, layer->fb_base[0]);
		regmap_update_bits(priv->regmap, MA35_CURSOR_CONFIG,
			MA35_CURSOR_FORMAT_MASK, MA35_CURSOR_FORMAT_A8R8G8B8);
	}

	/* update hotspot offset & format */
	if (old_state->hotspot_x != new_state->hotspot_x ||
		old_state->hotspot_y != new_state->hotspot_y) {
		reg = MA35_CURSOR_FORMAT_A8R8G8B8 |
			FIELD_PREP(MA35_CURSOR_HOTSPOT_X_MASK, new_state->hotspot_x) |
			FIELD_PREP(MA35_CURSOR_HOTSPOT_Y_MASK, new_state->hotspot_y);
		regmap_write(priv->regmap, MA35_CURSOR_CONFIG, reg);
		old_state->hotspot_x = new_state->hotspot_x;
		old_state->hotspot_y = new_state->hotspot_y;
	}
}

static void ma35_plane_atomic_disable(struct drm_plane *drm_plane,
					 struct drm_atomic_state *state)
{
	struct ma35_drm *priv = ma35_drm(drm_plane->dev);

	regmap_update_bits(priv->regmap, MA35_FRAMEBUFFER_CONFIG,
		MA35_PRIMARY_ENABLE, 0);
}

static void ma35_cursor_plane_atomic_disable(struct drm_plane *drm_plane,
					 struct drm_atomic_state *state)
{
	struct ma35_drm *priv = ma35_drm(drm_plane->dev);

	regmap_update_bits(priv->regmap, MA35_CURSOR_CONFIG,
		MA35_CURSOR_FORMAT_MASK, MA35_CURSOR_FORMAT_DISABLE);
}

static struct drm_plane_helper_funcs ma35_plane_helper_funcs = {
	.atomic_check		= ma35_plane_atomic_check,
	.atomic_update		= ma35_plane_atomic_update,
	.atomic_disable		= ma35_plane_atomic_disable,
};

static struct drm_plane_helper_funcs ma35_cursor_plane_helper_funcs = {
	.atomic_check		= ma35_cursor_plane_atomic_check,
	.atomic_update		= ma35_cursor_plane_atomic_update,
	.atomic_disable		= ma35_cursor_plane_atomic_disable,
	.atomic_async_check		= ma35_cursor_plane_atomic_async_check,
	.atomic_async_update	= ma35_cursor_plane_atomic_async_update,
};

static int ma35_plane_set_property(struct drm_plane *drm_plane,
	struct drm_plane_state *state, struct drm_property *property,
	uint64_t val)
{
	if (property == drm_plane->hotspot_x_property)
		state->hotspot_x = val;
	else if (property == drm_plane->hotspot_y_property)
		state->hotspot_y = val;
	else
		return -EINVAL;

	return 0;
}

static int ma35_plane_get_property(struct drm_plane *drm_plane,
	const struct drm_plane_state *state, struct drm_property *property,
	uint64_t *val)
{
	if (property == drm_plane->hotspot_x_property)
		*val = state->hotspot_x;
	else if (property == drm_plane->hotspot_y_property)
		*val = state->hotspot_y;
	else
		return -EINVAL;

	return 0;
}

static const struct drm_plane_funcs ma35_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
	.atomic_set_property = ma35_plane_set_property,
	.atomic_get_property = ma35_plane_get_property,
};

static int ma35_layer_create_properties(struct ma35_drm *priv,
				      struct ma35_layer *layer)
{
	struct drm_plane *drm_plane = &layer->drm_plane;
	int ret = 0;

	if (ma35_plane_properties[drm_plane->type].alpha) {
		drm_plane_create_alpha_property(drm_plane);
		ret = drm_plane_create_blend_mode_property(drm_plane, BIT(DRM_MODE_BLEND_PREMULTI) |
							 BIT(DRM_MODE_BLEND_COVERAGE));
		if (ret)
			return ret;
	}

	return ret;
}

struct ma35_layer *ma35_layer_get_from_type(struct ma35_drm *priv, enum drm_plane_type type)
{
	struct ma35_layer *layer;
	struct drm_plane *drm_plane;

	list_for_each_entry(layer, &priv->layers_list, list) {
		drm_plane = &layer->drm_plane;
		if (drm_plane->type == type)
			return layer;
	}

	return NULL;
}

static int ma35_layer_create(struct ma35_drm *priv,
			      struct device_node *of_node, u32 index,
			      enum drm_plane_type type)
{
	struct drm_device *drm_dev = &priv->drm_dev;
	struct device *dev = drm_dev->dev;
	struct ma35_layer *layer;
	int ret;

	layer = drmm_kzalloc(drm_dev, sizeof(*layer), GFP_KERNEL);
	if (!layer) {
		ret = -ENOMEM;
		goto error;
	}

	layer->of_node = of_node;

	if (type == DRM_PLANE_TYPE_CURSOR) {
		ret = drm_universal_plane_init(drm_dev, &layer->drm_plane,
			1 << MA35_DEFAULT_CRTC_ID,
			&ma35_plane_funcs, ma35_cursor_formats,
			ARRAY_SIZE(ma35_cursor_formats), NULL, type, NULL);
		if (ret) {
			drm_err(drm_dev, "Failed to initialize layer plane\n");
			return ret;
		}

		drm_plane_helper_add(&layer->drm_plane, &ma35_cursor_plane_helper_funcs);
	} else {
		ret = drm_universal_plane_init(drm_dev, &layer->drm_plane,
			1 << MA35_DEFAULT_CRTC_ID,
			&ma35_plane_funcs, ma35_layer_formats,
			ARRAY_SIZE(ma35_layer_formats), NULL, type, NULL);
		if (ret) {
			drm_err(drm_dev, "Failed to initialize layer plane\n");
			return ret;
		}

		drm_plane_helper_add(&layer->drm_plane, &ma35_plane_helper_funcs);
	}

	if (ma35_layer_create_properties(priv, layer))
		drm_warn(drm_dev, "Failed to create properties for layer #%d\n",
			index);

	drm_plane_create_zpos_immutable_property(&layer->drm_plane, index);

	list_add_tail(&layer->list, &priv->layers_list);

	return 0;

error:
	if (layer) {
		list_del(&layer->list);
		devm_kfree(dev, layer);
	}

	return ret;
}

void ma35_overlay_attach_crtc(struct ma35_drm *priv)
{
	uint32_t possible_crtcs = drm_crtc_mask(&priv->crtc->drm_crtc);
	struct ma35_layer *layer;
	struct drm_plane *drm_plane;

	list_for_each_entry(layer, &priv->layers_list, list) {
		drm_plane = &layer->drm_plane;
		if (drm_plane->type != DRM_PLANE_TYPE_OVERLAY)
			continue;

		drm_plane->possible_crtcs = possible_crtcs;
	}
}

int ma35_plane_init(struct ma35_drm *priv)
{
	struct drm_device *drm_dev = &priv->drm_dev;
	int ret;

	ret = ma35_layer_create(priv, NULL, 0, DRM_PLANE_TYPE_PRIMARY);
	if (ret) {
		drm_err(drm_dev, "Failed to create primary layer\n");
		return ret;
	}

	ret = ma35_layer_create(priv, NULL, 1, DRM_PLANE_TYPE_OVERLAY);
	if (ret) {
		drm_err(drm_dev, "Failed to create overlay layer\n");
		return ret;
	}

	ret = ma35_layer_create(priv, NULL, 2, DRM_PLANE_TYPE_CURSOR);
	if (ret) {
		drm_err(drm_dev, "Failed to create cursor layer\n");
		return ret;
	}

	return 0;
}
