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

/* Init Helpers */

int drm_colorop_init(struct drm_device *dev, struct drm_colorop *colorop,
		     struct drm_plane *plane)
{
	struct drm_mode_config *config = &dev->mode_config;
	int ret = 0;

	ret = drm_mode_object_add(dev, &colorop->base, DRM_MODE_OBJECT_COLOROP);
	if (ret)
		return ret;

	colorop->base.properties = &colorop->properties;
	colorop->dev = dev;
	colorop->plane = plane;

	list_add_tail(&colorop->head, &config->colorop_list);
	colorop->index = config->num_colorop++;

	/* add properties */
	return ret;
}
EXPORT_SYMBOL(drm_colorop_init);

static void __drm_atomic_helper_colorop_duplicate_state(struct drm_colorop *colorop,
							struct drm_colorop_state *state)
{
	memcpy(state, colorop->state, sizeof(*state));
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
static void __drm_colorop_state_reset(struct drm_colorop_state *colorop_state,
				      struct drm_colorop *colorop)
{
	colorop_state->colorop = colorop;
}

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
static void __drm_colorop_reset(struct drm_colorop *colorop,
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
	kfree(colorop->state);
	colorop->state = kzalloc(sizeof(*colorop->state), GFP_KERNEL);

	if (colorop->state)
		__drm_colorop_reset(colorop, colorop->state);
}
EXPORT_SYMBOL(drm_colorop_reset);
