/*
 * Copyright (C) 2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include <drm/drm_colorop.h>
#include <drm/drm_print.h>
#include <drm/drm_drv.h>
#include <drm/drm_plane.h>

#include "drm_crtc_internal.h"

/* TODO big colorop doc, including properties, etc. */

static const struct drm_prop_enum_list drm_colorop_type_enum_list[] = {
	{ DRM_COLOROP_1D_CURVE, "1D Curve" },
};

static const struct drm_prop_enum_list drm_colorop_curve_1d_type_enum_list[] = {
	{ DRM_COLOROP_1D_CURVE_SRGB_EOTF, "sRGB EOTF" },
	{ DRM_COLOROP_1D_CURVE_SRGB_INV_EOTF, "sRGB Inverse EOTF" },
};

/* Init Helpers */

int drm_colorop_init(struct drm_device *dev, struct drm_colorop *colorop,
		     struct drm_plane *plane, enum drm_colorop_type type)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_property *prop;
	int ret = 0;

	ret = drm_mode_object_add(dev, &colorop->base, DRM_MODE_OBJECT_COLOROP);
	if (ret)
		return ret;

	colorop->base.properties = &colorop->properties;
	colorop->dev = dev;
	colorop->type = type;
	colorop->plane = plane;

	list_add_tail(&colorop->head, &config->colorop_list);
	colorop->index = config->num_colorop++;

	/* add properties */

	/* type */
	prop = drm_property_create_enum(dev,
					DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_ATOMIC,
					"TYPE", drm_colorop_type_enum_list,
					ARRAY_SIZE(drm_colorop_type_enum_list));
	if (!prop)
		return -ENOMEM;

	colorop->type_property = prop;

	drm_object_attach_property(&colorop->base,
				   colorop->type_property,
				   colorop->type);

	/* bypass */
	/* TODO can we reuse the mode_config->active_prop? */
	prop = drm_property_create_bool(dev, DRM_MODE_PROP_ATOMIC,
					"BYPASS");
	if (!prop)
		return -ENOMEM;

	colorop->bypass_property = prop;
	drm_object_attach_property(&colorop->base,
				   colorop->bypass_property,
				   1);

	/* curve_1d_type */
	/* TODO move to mode_config? */
	prop = drm_property_create_enum(dev, DRM_MODE_PROP_ATOMIC,
					"CURVE_1D_TYPE",
					drm_colorop_curve_1d_type_enum_list,
					ARRAY_SIZE(drm_colorop_curve_1d_type_enum_list));
	if (!prop)
		return -ENOMEM;

	colorop->curve_1d_type_property = prop;
	drm_object_attach_property(&colorop->base,
				   colorop->curve_1d_type_property,
				   0);

	return ret;
}
EXPORT_SYMBOL(drm_colorop_init);

void __drm_atomic_helper_colorop_duplicate_state(struct drm_colorop *colorop,
						 struct drm_colorop_state *state)
{
	memcpy(state, colorop->state, sizeof(*state));

	state->bypass = true;
}

struct drm_colorop_state *
drm_atomic_helper_colorop_duplicate_state(struct drm_colorop *colorop)
{
	struct drm_colorop_state *state;

	if (WARN_ON(!colorop->state))
		return NULL;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (state)
		__drm_atomic_helper_colorop_duplicate_state(colorop, state);

	return state;
}


void drm_colorop_atomic_destroy_state(struct drm_colorop *colorop,
				      struct drm_colorop_state *state)
{
	kfree(state);
}

/**
 * __drm_colorop_destroy_state - release colorop state
 * @state: colorop state object to release
 *
 * Releases all resources stored in the colorop state without actually freeing
 * the memory of the colorop state. This is useful for drivers that subclass the
 * colorop state.
 */
void __drm_colorop_destroy_state(struct drm_colorop_state *state)
{
	/* TODO might need this later */
}

/**
 * drm_colorop_destroy_state - default state destroy hook
 * @colorop: drm colorop
 * @state: colorop state object to release
 *
 * Default colorop state destroy hook for drivers which don't have their own
 * subclassed colorop state structure.
 */
void drm_colorop_destroy_state(struct drm_colorop *colorop,
					   struct drm_colorop_state *state)
{
	kfree(state);
}
EXPORT_SYMBOL(drm_colorop_destroy_state);

/**
 * __drm_colorop_state_reset - resets colorop state to default values
 * @colorop_state: atomic colorop state, must not be NULL
 * @colorop: colorop object, must not be NULL
 *
 * Initializes the newly allocated @colorop_state with default
 * values. This is useful for drivers that subclass the CRTC state.
 */
void __drm_colorop_state_reset(struct drm_colorop_state *colorop_state,
					   struct drm_colorop *colorop)
{
	colorop_state->colorop = colorop;
	colorop_state->bypass = true;
}
EXPORT_SYMBOL(__drm_colorop_state_reset);

/**
 * __drm_colorop_reset - reset state on colorop
 * @colorop: drm colorop
 * @colorop_state: colorop state to assign
 *
 * Initializes the newly allocated @colorop_state and assigns it to
 * the &drm_crtc->state pointer of @colorop, usually required when
 * initializing the drivers or when called from the &drm_colorop_funcs.reset
 * hook.
 *
 * This is useful for drivers that subclass the colorop state.
 */
void __drm_colorop_reset(struct drm_colorop *colorop,
				     struct drm_colorop_state *colorop_state)
{
	if (colorop_state)
		__drm_colorop_state_reset(colorop_state, colorop);

	colorop->state = colorop_state;
}

/**
 * drm_colorop_reset - reset colorop atomic state
 * @colorop: drm colorop
 *
 * Resets the atomic state for @colorop by freeing the state pointer (which might
 * be NULL, e.g. at driver load time) and allocating a new empty state object.
 */
void drm_colorop_reset(struct drm_colorop *colorop)
{
	if (colorop->state)
		__drm_colorop_destroy_state(colorop->state);

	kfree(colorop->state);
	colorop->state = kzalloc(sizeof(*colorop->state), GFP_KERNEL);

	if (colorop->state)
		__drm_colorop_reset(colorop, colorop->state);
}
EXPORT_SYMBOL(drm_colorop_reset);


static const char * const colorop_type_name[] = {
	[DRM_COLOROP_1D_CURVE] = "1D Curve",
};

static const char * const colorop_curve_1d_type_name[] = {
	[DRM_COLOROP_1D_CURVE_SRGB_EOTF] = "sRGB EOTF",
	[DRM_COLOROP_1D_CURVE_SRGB_INV_EOTF] = "sRGB Inverse EOTF",
};

/**
 * drm_get_colorop_type_name - return a string for colorop type
 * @range: colorop type to compute name of
 *
 * In contrast to the other drm_get_*_name functions this one here returns a
 * const pointer and hence is threadsafe.
 */
const char *drm_get_colorop_type_name(enum drm_colorop_type type)
{
	if (WARN_ON(type >= ARRAY_SIZE(colorop_type_name)))
		return "unknown";

	return colorop_type_name[type];
}

/**
 * drm_get_colorop_curve_1d_type_name - return a string for 1D curve type
 * @range: 1d curve type to compute name of
 *
 * In contrast to the other drm_get_*_name functions this one here returns a
 * const pointer and hence is threadsafe.
 */
const char *drm_get_colorop_curve_1d_type_name(enum drm_colorop_curve_1d_type type)
{
	if (WARN_ON(type >= ARRAY_SIZE(colorop_curve_1d_type_name)))
		return "unknown";

	return colorop_curve_1d_type_name[type];
}
