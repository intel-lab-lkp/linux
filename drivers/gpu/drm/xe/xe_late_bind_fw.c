// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <linux/component.h>
#include <linux/delay.h>
#include <linux/firmware.h>

#include <drm/drm_managed.h>
#include <drm/intel/i915_component.h>
#include <drm/intel/late_bind_mei_interface.h>
#include <drm/drm_print.h>

#include "xe_device.h"
#include "xe_late_bind_fw.h"
#include "xe_pcode.h"
#include "xe_pcode_api.h"

static const char * const fw_type_to_name[] = {
		[CSC_LATE_BINDING_TYPE_FAN_CONTROL] = "fan_control",
	};

static struct xe_device *late_bind_to_xe(struct xe_late_bind *late_bind)
{
	return container_of(late_bind, struct xe_device, late_bind);
}

static int late_bind_fw_num_fans(struct xe_late_bind *late_bind)
{
	struct xe_device *xe = late_bind_to_xe(late_bind);
	struct xe_tile *root_tile = xe_device_get_root_tile(xe);
	u32 uval;

	if (!xe_pcode_read(root_tile,
			   PCODE_MBOX(FAN_SPEED_CONTROL, FSC_READ_NUM_FANS, 0), &uval, NULL))
		return uval;
	else
		return 0;
}

static int late_bind_fw_init(struct xe_late_bind *late_bind, u32 type)
{
	struct xe_device *xe = late_bind_to_xe(late_bind);
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct xe_late_bind_fw *lb_fw;
	const struct firmware *fw;
	u32 num_fans;
	int ret;

	if (!late_bind->component_added)
		return 0;

	lb_fw = &late_bind->late_bind_fw;

	lb_fw->type = type;
	lb_fw->valid = false;

	if (lb_fw->type == CSC_LATE_BINDING_TYPE_FAN_CONTROL) {
		num_fans = late_bind_fw_num_fans(late_bind);
		drm_dbg(&xe->drm, "Number of Fans: %d\n", num_fans);
		if (!num_fans)
			return 0;
	}

	lb_fw->flags = CSC_LATE_BINDING_FLAGS_IS_PERSISTENT;

	snprintf(lb_fw->blob_path, sizeof(lb_fw->blob_path), "xe/%s_8086_%04x_%04x_%04x.bin",
		 fw_type_to_name[type], pdev->device,
		 pdev->subsystem_vendor, pdev->subsystem_device);

	drm_dbg(&xe->drm, "Request late binding firmware %s\n", lb_fw->blob_path);
	ret = firmware_request_nowarn(&fw, lb_fw->blob_path, xe->drm.dev);
	if (ret) {
		drm_dbg(&xe->drm, "Failed to request %s\n", lb_fw->blob_path);
		return 0;
	}

	if (fw->size > MAX_PAYLOAD_SIZE) {
		lb_fw->payload_size = MAX_PAYLOAD_SIZE;
		drm_err(&xe->drm, "Firmware %s size %zu is larger than max pay load size %u\n",
			lb_fw->blob_path, fw->size, MAX_PAYLOAD_SIZE);
		return 0;
	}

	lb_fw->payload_size = fw->size;

	memcpy(lb_fw->payload, fw->data, lb_fw->payload_size);
	release_firmware(fw);
	lb_fw->valid = true;

	return 0;
}

/**
 * xe_mei_late_bind_fw_init() - Initialize late bind firmware
 *
 * Return: 0 if the initialization was successful, a negative errno otherwise.
 */
int xe_late_bind_fw_init(struct xe_late_bind *late_bind)
{
	return late_bind_fw_init(late_bind, CSC_LATE_BINDING_TYPE_FAN_CONTROL);
}

static int xe_late_bind_component_bind(struct device *xe_kdev,
				       struct device *mei_kdev, void *data)
{
	struct xe_device *xe = kdev_to_xe_device(xe_kdev);
	struct xe_late_bind *late_bind = &xe->late_bind;

	mutex_lock(&late_bind->mutex);
	late_bind->component.ops = data;
	late_bind->component.mei_dev = mei_kdev;
	mutex_unlock(&late_bind->mutex);

	return 0;
}

static void xe_late_bind_component_unbind(struct device *xe_kdev,
					  struct device *mei_kdev, void *data)
{
	struct xe_device *xe = kdev_to_xe_device(xe_kdev);
	struct xe_late_bind *late_bind = &xe->late_bind;

	mutex_lock(&late_bind->mutex);
	late_bind->component.ops = NULL;
	mutex_unlock(&late_bind->mutex);
}

static const struct component_ops xe_late_bind_component_ops = {
	.bind   = xe_late_bind_component_bind,
	.unbind = xe_late_bind_component_unbind,
};

static void xe_late_bind_remove(void *arg)
{
	struct xe_late_bind *late_bind = arg;
	struct xe_device *xe = late_bind_to_xe(late_bind);

	if (!late_bind->component_added)
		return;

	component_del(xe->drm.dev, &xe_late_bind_component_ops);
	late_bind->component_added = false;
	mutex_destroy(&late_bind->mutex);
}

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

	if (!IS_ENABLED(CONFIG_INTEL_MEI_LATE_BIND) || !IS_ENABLED(CONFIG_INTEL_MEI_GSC)) {
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

	return devm_add_action_or_reset(xe->drm.dev, xe_late_bind_remove, late_bind);
}
