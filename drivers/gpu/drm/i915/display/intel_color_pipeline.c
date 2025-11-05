// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */
#include "intel_colorop.h"
#include "intel_color_pipeline.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "skl_universal_plane.h"

#define MAX_COLOR_PIPELINES 5
#define PLANE_DEGAMMA_SIZE 128
#define PLANE_GAMMA_SIZE 32

static
int _intel_color_pipeline_plane_init(struct drm_plane *plane, struct drm_prop_enum_list *list)
{
	struct intel_colorop *colorop;
	struct drm_device *dev = plane->dev;
	int ret;
	struct drm_colorop *prev_op;

	colorop = intel_colorop_create(INTEL_PLANE_CB_PRE_CSC_LUT);

	ret = drm_plane_colorop_curve_1d_lut_init(dev, &colorop->base, plane,
						  PLANE_DEGAMMA_SIZE,
						  DRM_COLOROP_LUT1D_INTERPOLATION_LINEAR,
						  DRM_COLOROP_FLAG_ALLOW_BYPASS);

	if (ret)
		return ret;

	list->type = colorop->base.base.id;
	list->name = kasprintf(GFP_KERNEL, "Color Pipeline %d", colorop->base.base.id);

	/* TODO: handle failures and clean up */
	prev_op = &colorop->base;

	colorop = intel_colorop_create(INTEL_PLANE_CB_CSC);
	ret = drm_plane_colorop_ctm_3x4_init(dev, &colorop->base, plane,
					     DRM_COLOROP_FLAG_ALLOW_BYPASS);
	if (ret)
		return ret;

	drm_colorop_set_next_property(prev_op, &colorop->base);
	prev_op = &colorop->base;

	colorop = intel_colorop_create(INTEL_PLANE_CB_POST_CSC_LUT);
	ret = drm_plane_colorop_curve_1d_lut_init(dev, &colorop->base, plane,
						  PLANE_GAMMA_SIZE,
						  DRM_COLOROP_LUT1D_INTERPOLATION_LINEAR,
						  DRM_COLOROP_FLAG_ALLOW_BYPASS);
	if (ret)
		return ret;

	drm_colorop_set_next_property(prev_op, &colorop->base);

	return 0;
}

int intel_color_pipeline_plane_init(struct drm_plane *plane)
{
	struct drm_device *dev = plane->dev;
	struct intel_display *display = to_intel_display(dev);
	struct drm_property *prop;
	struct drm_prop_enum_list pipelines[MAX_COLOR_PIPELINES];
	int len = 0;
	int ret;

	/* Currently expose pipeline only for HDR planes */
	if (!icl_is_hdr_plane(display, to_intel_plane(plane)->id))
		return 0;

	/* Add "Bypass" (i.e. NULL) pipeline */
	pipelines[len].type = 0;
	pipelines[len].name = "Bypass";
	len++;

	/* Add pipeline consisting of transfer functions */
	ret = _intel_color_pipeline_plane_init(plane, &pipelines[len]);
	if (ret)
		return ret;
	len++;

	/* Create COLOR_PIPELINE property and attach */
	prop = drm_property_create_enum(dev, DRM_MODE_PROP_ATOMIC,
					"COLOR_PIPELINE",
					pipelines, len);
	if (!prop)
		return -ENOMEM;

	plane->color_pipeline_property = prop;

	drm_object_attach_property(&plane->base, prop, 0);

	/* TODO check if needed */
	if (plane->state)
		plane->state->color_pipeline = NULL;

	return 0;
}
