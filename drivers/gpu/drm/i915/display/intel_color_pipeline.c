// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */
#include <drm/drm_print.h>

#include "intel_color.h"
#include "intel_colorop.h"
#include "intel_color_pipeline.h"
#include "intel_de.h"
#include "intel_display_types.h"
#include "skl_universal_plane.h"

#define MAX_COLOR_PIPELINES 1
#define MAX_COLOROP 4
#define PLANE_DEGAMMA_SIZE 128
#define PLANE_GAMMA_SIZE 32

static const struct drm_colorop_funcs intel_colorop_funcs = {
	.destroy = intel_colorop_destroy,
};

static
struct intel_colorop *intel_color_pipeline_plane_add_colorop(struct drm_plane *plane,
							     struct intel_colorop *prev,
							     enum intel_color_block id)
{
	struct drm_device *dev = plane->dev;
	struct intel_colorop *colorop;
	int ret;

	colorop = intel_colorop_create(id);

	if (IS_ERR(colorop))
		return colorop;

	switch (id) {
	case INTEL_PLANE_CB_PRE_CSC_LUT:
		ret = drm_plane_colorop_curve_1d_lut_init(dev,
							  &colorop->base, plane,
							  &intel_colorop_funcs,
							  PLANE_DEGAMMA_SIZE,
							  DRM_COLOROP_LUT1D_INTERPOLATION_LINEAR,
							  DRM_COLOROP_FLAG_ALLOW_BYPASS);
		break;
	case INTEL_PLANE_CB_CSC:
		ret = drm_plane_colorop_ctm_3x4_init(dev, &colorop->base, plane,
						     &intel_colorop_funcs,
						     DRM_COLOROP_FLAG_ALLOW_BYPASS);
		break;
	case INTEL_PLANE_CB_3DLUT:
		ret = drm_plane_colorop_3dlut_init(dev, &colorop->base, plane,
						   &intel_colorop_funcs, 17,
						   DRM_COLOROP_LUT3D_INTERPOLATION_TETRAHEDRAL,
						   true);
		break;
	case INTEL_PLANE_CB_POST_CSC_LUT:
		ret = drm_plane_colorop_curve_1d_lut_init(dev, &colorop->base, plane,
							  &intel_colorop_funcs,
							  PLANE_GAMMA_SIZE,
							  DRM_COLOROP_LUT1D_INTERPOLATION_LINEAR,
							  DRM_COLOROP_FLAG_ALLOW_BYPASS);
		break;
	default:
		drm_err(plane->dev, "Invalid colorop id [%d]", id);
		ret = -EINVAL;
	}

	if (ret)
		goto cleanup;

	if (prev)
		drm_colorop_set_next_property(&prev->base, &colorop->base);

	return colorop;

cleanup:
	intel_colorop_destroy(&colorop->base);
	return ERR_PTR(ret);
}

static
int _intel_color_pipeline_plane_init(struct drm_plane *plane, struct drm_prop_enum_list *list,
				     enum pipe pipe)
{
	struct drm_device *dev = plane->dev;
	struct intel_display *display = to_intel_display(dev);
	struct intel_colorop *colorop[MAX_COLOROP];
	int ret = 0;
	int i = 0;

	colorop[i] = intel_color_pipeline_plane_add_colorop(plane, NULL,
							    INTEL_PLANE_CB_PRE_CSC_LUT);

	if (IS_ERR(colorop[i])) {
		ret = PTR_ERR(colorop[i]);
		goto cleanup;
	}
	i++;


	colorop[i] = intel_color_pipeline_plane_add_colorop(plane, colorop[i - 1],
							    INTEL_PLANE_CB_CSC);

	if (IS_ERR(colorop[i])) {
		ret = PTR_ERR(colorop[i]);
		goto cleanup;
	}

	i++;

	if (DISPLAY_VER(display) >= 35 &&
	    intel_color_crtc_has_3dlut(display, pipe) &&
	    plane->type == DRM_PLANE_TYPE_PRIMARY) {
		colorop[i] = intel_color_pipeline_plane_add_colorop(plane, colorop[i - 1],
								    INTEL_PLANE_CB_3DLUT);

		if (IS_ERR(colorop[i])) {
			ret = PTR_ERR(colorop[i]);
			goto cleanup;
		}
		i++;
	}

	colorop[i] = intel_color_pipeline_plane_add_colorop(plane, colorop[i - 1],
							    INTEL_PLANE_CB_POST_CSC_LUT);
	if (IS_ERR(colorop[i])) {
		ret = PTR_ERR(colorop[i]);
		goto cleanup;
	}
	i++;

	list->type = colorop[0]->base.base.id;
	list->name = kasprintf(GFP_KERNEL, "Color Pipeline %d", colorop[0]->base.base.id);

	return 0;

cleanup:
	while (--i >= 0)
		intel_colorop_destroy(&colorop[i]->base);
	return ret;
}

int intel_color_pipeline_plane_init(struct drm_plane *plane, enum pipe pipe)
{
	struct drm_device *dev = plane->dev;
	struct intel_display *display = to_intel_display(dev);
	struct drm_prop_enum_list pipelines[MAX_COLOR_PIPELINES] = {};
	int len = 0;
	int ret = 0;
	int i;

	/* Currently expose pipeline only for HDR planes */
	if (!icl_is_hdr_plane(display, to_intel_plane(plane)->id))
		return 0;

	/* Add pipeline consisting of transfer functions */
	ret = _intel_color_pipeline_plane_init(plane, &pipelines[len], pipe);
	if (ret)
		goto out;
	len++;

	ret = drm_plane_create_color_pipeline_property(plane, pipelines, len);

	for (i = 0; i < len; i++)
		kfree(pipelines[i].name);

out:
	return ret;
}
