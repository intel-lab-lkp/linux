/* SPDX-License-Identifier: GPL-2.0+ */

#include <linux/slab.h>
#include <drm/drm_colorop.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>
#include <drm/drm_plane.h>

#define MAX_COLOR_PIPELINES 5

const int vkms_initialize_tf_pipeline(struct drm_plane *plane, struct drm_prop_enum_list *list)
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

	ret = drm_colorop_init(dev, op, plane, DRM_COLOROP_1D_CURVE);
	if (ret)
		return ret;

	list->type = op->base.id;
	list->name = kasprintf(GFP_KERNEL, "Color Pipeline %d", op->base.id);

	prev_op = op;

	/* 2nd op: 1d curve */
	op = kzalloc(sizeof(struct drm_colorop), GFP_KERNEL);
	if (!op) {
		DRM_ERROR("KMS: Failed to allocate colorop\n");
		return -ENOMEM;
	}

	ret = drm_colorop_init(dev, op, plane, DRM_COLOROP_1D_CURVE);
	if (ret)
		return ret;

	drm_colorop_set_next_property(prev_op, op);

	return 0;
}

int vkms_initialize_colorops(struct drm_plane *plane)
{
	struct drm_device *dev = plane->dev;
	struct drm_property *prop;
	struct drm_prop_enum_list pipelines[MAX_COLOR_PIPELINES];
	int len = 0;
	int ret;

	/* Add "Bypass" (i.e. NULL) pipeline */
	pipelines[len].type = 0;
	pipelines[len].name = "Bypass";
	len++;

	/* Add pipeline consisting of transfer functions */
	ret = vkms_initialize_tf_pipeline(plane, &(pipelines[len]));
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

	/* TODO do we even need this? */
	if (plane->state)
		plane->state->color_pipeline = NULL;

	return 0;
}
