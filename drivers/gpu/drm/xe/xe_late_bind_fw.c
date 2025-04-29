// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <linux/component.h>
#include <linux/delay.h>

#include <drm/drm_managed.h>
#include <drm/intel/i915_component.h>
#include <drm/intel/xe_late_bind_mei_interface.h>
#include <drm/drm_print.h>

#include "xe_device.h"
#include "xe_late_bind_fw.h"

static struct xe_device *
late_bind_to_xe(struct xe_late_bind *late_bind)
{
	return container_of(late_bind, struct xe_device, late_bind);
}

static int xe_late_bind_component_bind(struct device *xe_kdev,
				       struct device *mei_kdev, void *data)
{
	struct xe_device *xe = kdev_to_xe_device(xe_kdev);
	struct xe_late_bind *late_bind = &xe->late_bind;
	struct xe_late_bind_component *component;

	component = drmm_kzalloc(&xe->drm, sizeof(*component), GFP_KERNEL);

	mutex_lock(&late_bind->mutex);
	component->mei_dev = mei_kdev;
	component->ops = data;
	mutex_unlock(&late_bind->mutex);

	late_bind->component = component;

	return 0;
}

static void xe_late_bind_component_unbind(struct device *xe_kdev,
					  struct device *mei_kdev, void *data)
{
	struct xe_device *xe = kdev_to_xe_device(xe_kdev);
	struct xe_late_bind *late_bind = &xe->late_bind;

	mutex_lock(&late_bind->mutex);
	late_bind->component = NULL;
	mutex_unlock(&late_bind->mutex);
}

static const struct component_ops xe_late_bind_component_ops = {
	.bind   = xe_late_bind_component_bind,
	.unbind = xe_late_bind_component_unbind,
};

/**
 * xe_late_bind_init() - add xe mei late binding component
 *
 * Return: 0 if the initialization was successful, a negative errno otherwise.
 */
int xe_late_bind_init(struct xe_late_bind *late_bind)
{
	struct xe_device *xe = late_bind_to_xe(late_bind);
	int err;

	if (xe->info.platform != XE_BATTLEMAGE)
		return 0;

	mutex_init(&late_bind->mutex);

	if (!IS_ENABLED(CONFIG_INTEL_MEI_LATE_BIND)) {
		drm_info(&xe->drm, "Can't init xe mei late bind missing mei component\n");
		return -ENODEV;
	}

	err = component_add_typed(xe->drm.dev, &xe_late_bind_component_ops,
				  I915_COMPONENT_LATE_BIND);
	if (err < 0) {
		drm_info(&xe->drm, "Failed to add mei late bind component (%pe)\n", ERR_PTR(err));
		return err;
	}

	late_bind->component_added = true;

	/* the component must be removed before unload, so can't use drmm for cleanup */

	return 0;
}

/**
 * xe_late_bind_remove() - remove the xe mei late binding component
 */
void xe_late_bind_remove(struct xe_late_bind *late_bind)
{
	struct xe_device *xe = late_bind_to_xe(late_bind);

	if (!late_bind->component_added)
		return;

	component_del(xe->drm.dev, &xe_late_bind_component_ops);
	late_bind->component_added = false;
}
