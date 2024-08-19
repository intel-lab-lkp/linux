/* SPDX-License-Identifier: GPL-2.0+ */

#include <linux/slab.h>
#include <drm/drm_colorop.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>
#include <drm/drm_plane.h>

#include "vkms_drv.h"

#define MAX_COLOR_PIPELINES 5

static const u64 supported_tfs =
	BIT(DRM_COLOROP_1D_CURVE_SRGB_EOTF) |
	BIT(DRM_COLOROP_1D_CURVE_SRGB_INV_EOTF);

static int vkms_initialize_color_pipeline(struct drm_plane *plane, struct drm_prop_enum_list *list)
{

	struct drm_colorop *op, *prev_op;
	struct drm_device *dev = plane->dev;
	int ret;

	/* 1st op: 1d curve */
	op = kzalloc(sizeof(struct drm_colorop), GFP_KERNEL);
	if (!op) {
		DRM_ERROR("KMS: Failed to allocate colorop\n");
		return -ENOMEM;
	}

	ret = drm_colorop_curve_1d_init(dev, op, plane, supported_tfs, true);
	if (ret)
		return ret;

	list->type = op->base.id;
	list->name = kasprintf(GFP_KERNEL, "Color Pipeline %d", op->base.id);

	prev_op = op;

	/* 2nd op: 3x4 matrix */
	op = kzalloc(sizeof(struct drm_colorop), GFP_KERNEL);
	if (!op) {
		DRM_ERROR("KMS: Failed to allocate colorop\n");
		return -ENOMEM;
	}

	ret = drm_colorop_ctm_3x4_init(dev, op, plane, true);
	if (ret)
		return ret;

	drm_colorop_set_next_property(prev_op, op);

	prev_op = op;

	/* 3rd op: 3x4 matrix */
	op = kzalloc(sizeof(struct drm_colorop), GFP_KERNEL);
	if (!op) {
		DRM_ERROR("KMS: Failed to allocate colorop\n");
		return -ENOMEM;
	}

	ret = drm_colorop_ctm_3x4_init(dev, op, plane, true);
	if (ret)
		return ret;

	drm_colorop_set_next_property(prev_op, op);

	prev_op = op;

	/* 4th op: 1d curve */
	op = kzalloc(sizeof(struct drm_colorop), GFP_KERNEL);
	if (!op) {
		DRM_ERROR("KMS: Failed to allocate colorop\n");
		return -ENOMEM;
	}

	ret = drm_colorop_curve_1d_init(dev, op, plane, supported_tfs, true);
	if (ret)
		return ret;

	drm_colorop_set_next_property(prev_op, op);

	return 0;
}

int vkms_initialize_colorops(struct drm_plane *plane)
{
	struct drm_prop_enum_list pipelines[MAX_COLOR_PIPELINES];
	int len = 0;
	int ret;

	/* Add color pipeline */
	ret = vkms_initialize_color_pipeline(plane, &(pipelines[len]));
	if (ret)
		return ret;
	len++;

	/* Create COLOR_PIPELINE property and attach */
	drm_plane_create_color_pipeline_property(plane, pipelines, len);

	return 0;
}
