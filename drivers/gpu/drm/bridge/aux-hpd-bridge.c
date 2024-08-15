// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Linaro Ltd.
 *
 * Author: Dmitry Baryshkov <dmitry.baryshkov@linaro.org>
 */
#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_dp.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_print.h>
#include <drm/bridge/aux-bridge.h>

static DEFINE_IDA(drm_aux_hpd_bridge_ida);

struct drm_aux_hpd_bridge_data {
	struct drm_bridge bridge;
	struct device *dev;
	bool no_hpd;
};

enum dp_lane {
	DP_ML0 = 0,	/* DP pins 1/3 */
	DP_ML1 = 1,	/* DP pins 4/6 */
	DP_ML2 = 2,	/* DP pins 7/9 */
	DP_ML3 = 3,	/* DP pins 10/12 */
};

struct drm_dp_typec_bridge_data {
	u8 dp_lanes[DP_ML3 + 1];
	size_t num_lanes;
	struct drm_aux_hpd_bridge_data hpd_bridge;
};

static inline struct drm_dp_typec_bridge_data *
hpd_bridge_to_typec_bridge_data(struct drm_aux_hpd_bridge_data *hpd_data)
{
	return container_of(hpd_data, struct drm_dp_typec_bridge_data, hpd_bridge);
}

static inline struct drm_dp_typec_bridge_data *
to_drm_dp_typec_bridge_data(struct drm_bridge *bridge)
{
	struct drm_aux_hpd_bridge_data *hpd_data;

	hpd_data = container_of(bridge, struct drm_aux_hpd_bridge_data, bridge);

	return hpd_bridge_to_typec_bridge_data(hpd_data);
}

struct drm_dp_typec_bridge_dev {
	struct auxiliary_device adev;
	size_t max_lanes;
	bool no_hpd;
};

static inline struct drm_dp_typec_bridge_dev *
to_drm_dp_typec_bridge_dev(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	return container_of(adev, struct drm_dp_typec_bridge_dev, adev);
}

static void drm_aux_hpd_bridge_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);

	ida_free(&drm_aux_hpd_bridge_ida, adev->id);

	of_node_put(adev->dev.platform_data);
	of_node_put(adev->dev.of_node);

	kfree(adev);
}

static void drm_dp_typec_bridge_release(struct device *dev)
{
	struct drm_dp_typec_bridge_dev *typec_bridge_dev;
	struct auxiliary_device *adev;

	typec_bridge_dev = to_drm_dp_typec_bridge_dev(dev);
	adev = &typec_bridge_dev->adev;

	ida_free(&drm_aux_hpd_bridge_ida, adev->id);

	of_node_put(adev->dev.platform_data);
	of_node_put(adev->dev.of_node);

	kfree(typec_bridge_dev);
}

static void drm_aux_hpd_bridge_free_adev(void *_adev)
{
	auxiliary_device_uninit(_adev);
}

/**
 * devm_drm_dp_hpd_bridge_alloc - allocate a HPD DisplayPort bridge
 * @parent: device instance providing this bridge
 * @np: device node pointer corresponding to this bridge instance
 *
 * Creates a simple DRM bridge with the type set to
 * DRM_MODE_CONNECTOR_DisplayPort, which terminates the bridge chain and is
 * able to send the HPD events.
 *
 * Return: bridge auxiliary device pointer or an error pointer
 */
struct auxiliary_device *devm_drm_dp_hpd_bridge_alloc(struct device *parent, struct device_node *np)
{
	struct auxiliary_device *adev;
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev)
		return ERR_PTR(-ENOMEM);

	ret = ida_alloc(&drm_aux_hpd_bridge_ida, GFP_KERNEL);
	if (ret < 0) {
		kfree(adev);
		return ERR_PTR(ret);
	}

	adev->id = ret;
	adev->name = "dp_hpd_bridge";
	adev->dev.parent = parent;
	adev->dev.of_node = of_node_get(parent->of_node);
	adev->dev.release = drm_aux_hpd_bridge_release;
	adev->dev.platform_data = of_node_get(np);

	ret = auxiliary_device_init(adev);
	if (ret) {
		of_node_put(adev->dev.platform_data);
		of_node_put(adev->dev.of_node);
		ida_free(&drm_aux_hpd_bridge_ida, adev->id);
		kfree(adev);
		return ERR_PTR(ret);
	}

	ret = devm_add_action_or_reset(parent, drm_aux_hpd_bridge_free_adev, adev);
	if (ret)
		return ERR_PTR(ret);

	return adev;
}
EXPORT_SYMBOL_GPL(devm_drm_dp_hpd_bridge_alloc);

static void drm_aux_hpd_bridge_del_adev(void *_adev)
{
	auxiliary_device_delete(_adev);
}

/**
 * devm_drm_dp_hpd_bridge_add - register a HDP DisplayPort bridge
 * @dev: struct device to tie registration lifetime to
 * @adev: bridge auxiliary device to be registered
 *
 * Returns: zero on success or a negative errno
 */
int devm_drm_dp_hpd_bridge_add(struct device *dev, struct auxiliary_device *adev)
{
	int ret;

	ret = auxiliary_device_add(adev);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, drm_aux_hpd_bridge_del_adev, adev);
}
EXPORT_SYMBOL_GPL(devm_drm_dp_hpd_bridge_add);

/**
 * drm_dp_hpd_bridge_register - allocate and register a HDP DisplayPort bridge
 * @parent: device instance providing this bridge
 * @np: device node pointer corresponding to this bridge instance
 *
 * Return: device instance that will handle created bridge or an error pointer
 */
struct device *drm_dp_hpd_bridge_register(struct device *parent, struct device_node *np)
{
	struct auxiliary_device *adev;
	int ret;

	adev = devm_drm_dp_hpd_bridge_alloc(parent, np);
	if (IS_ERR(adev))
		return ERR_CAST(adev);

	ret = devm_drm_dp_hpd_bridge_add(parent, adev);
	if (ret)
		return ERR_PTR(ret);

	return &adev->dev;
}
EXPORT_SYMBOL_GPL(drm_dp_hpd_bridge_register);

/**
 * devm_drm_dp_typec_bridge_alloc - Allocate a USB type-C DisplayPort bridge
 * @parent: device instance providing this bridge
 * @desc: device node pointer corresponding to this bridge instance
 *
 * Creates a DRM bridge with the type set to DRM_MODE_CONNECTOR_DisplayPort,
 * which terminates the bridge chain and is able to send the HPD events along
 * with remap DP lanes to match USB type-c DP altmode pin assignments.
 *
 * Return: device instance that will handle created bridge or an error code
 * encoded into the pointer.
 */
struct drm_dp_typec_bridge_dev *
devm_drm_dp_typec_bridge_alloc(struct device *parent, const struct drm_dp_typec_bridge_desc *desc)
{
	struct drm_dp_typec_bridge_dev *typec_bridge_dev;
	struct auxiliary_device *adev;
	int ret;

	typec_bridge_dev = kzalloc(sizeof(*typec_bridge_dev), GFP_KERNEL);
	if (!typec_bridge_dev)
		return ERR_PTR(-ENOMEM);
	adev = &typec_bridge_dev->adev;

	ret = ida_alloc(&drm_aux_hpd_bridge_ida, GFP_KERNEL);
	if (ret < 0) {
		kfree(adev);
		return ERR_PTR(ret);
	}

	adev->id = ret;
	adev->name = "dp_typec_bridge";
	adev->dev.parent = parent;
	adev->dev.of_node = of_node_get(parent->of_node);
	adev->dev.release = drm_dp_typec_bridge_release;
	adev->dev.platform_data = of_node_get(desc->of_node);
	typec_bridge_dev->max_lanes = desc->num_dp_lanes;
	typec_bridge_dev->no_hpd = desc->no_hpd;

	ret = auxiliary_device_init(adev);
	if (ret) {
		of_node_put(adev->dev.platform_data);
		of_node_put(adev->dev.of_node);
		ida_free(&drm_aux_hpd_bridge_ida, adev->id);
		kfree(adev);
		return ERR_PTR(ret);
	}

	return typec_bridge_dev;
}
EXPORT_SYMBOL_GPL(devm_drm_dp_typec_bridge_alloc);

/**
 * devm_drm_dp_typec_bridge_add - register a USB type-C DisplayPort bridge
 * @dev: struct device to tie registration lifetime to
 * @typec_bridge_dev: USB type-c DisplayPort bridge to be registered
 *
 * Returns: zero on success or a negative errno
 */
int devm_drm_dp_typec_bridge_add(struct device *dev, struct drm_dp_typec_bridge_dev *typec_bridge_dev)
{
	struct auxiliary_device *adev = &typec_bridge_dev->adev;

	return devm_drm_dp_hpd_bridge_add(dev, adev);
}
EXPORT_SYMBOL_GPL(devm_drm_dp_typec_bridge_add);

/**
 * drm_aux_hpd_bridge_notify - notify hot plug detection events
 * @dev: device created for the HPD bridge
 * @status: output connection status
 *
 * A wrapper around drm_bridge_hpd_notify() that is used to report hot plug
 * detection events for bridges created via drm_dp_hpd_bridge_register().
 *
 * This function shall be called in a context that can sleep.
 */
void drm_aux_hpd_bridge_notify(struct device *dev, enum drm_connector_status status)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);
	struct drm_aux_hpd_bridge_data *data = auxiliary_get_drvdata(adev);

	if (!data)
		return;
	if (data->no_hpd)
		return;

	drm_bridge_hpd_notify(&data->bridge, status);
}
EXPORT_SYMBOL_GPL(drm_aux_hpd_bridge_notify);

/**
 * drm_dp_typec_bridge_notify - notify hot plug detection events
 * @typec_bridge_dev: device created for the type-C bridge
 * @status: output connection status
 *
 * A wrapper around drm_bridge_hpd_notify() that is used to report hot plug
 * detection events for bridges created via devm_drm_dp_typec_bridge_alloc().
 *
 * This function shall be called in a context that can sleep.
 */
void drm_dp_typec_bridge_notify(struct drm_dp_typec_bridge_dev *typec_bridge_dev,
				enum drm_connector_status status)
{
	drm_aux_hpd_bridge_notify(&typec_bridge_dev->adev.dev, status);
}
EXPORT_SYMBOL_GPL(drm_dp_typec_bridge_notify);

static int drm_aux_hpd_bridge_attach(struct drm_bridge *bridge,
				     enum drm_bridge_attach_flags flags)
{
	return flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR ? 0 : -EINVAL;
}

static int dp_lane_to_typec_lane(enum dp_lane lane)
{
	switch (lane) {
	case DP_ML0:
		return USB_SSTX2;
	case DP_ML1:
		return USB_SSRX2;
	case DP_ML2:
		return USB_SSTX1;
	case DP_ML3:
		return USB_SSRX1;
	}

	return -EINVAL;
}

static int typec_to_dp_lane(enum usb_ss_lane lane,
			    enum typec_orientation orientation)
{
	switch (orientation) {
	case TYPEC_ORIENTATION_NONE:
	case TYPEC_ORIENTATION_NORMAL:
		switch (lane) {
		case USB_SSRX1:
			return DP_ML3;
		case USB_SSTX1:
			return DP_ML2;
		case USB_SSTX2:
			return DP_ML0;
		case USB_SSRX2:
			return DP_ML1;
		}
		break;
	case TYPEC_ORIENTATION_REVERSE:
		switch (lane) {
		case USB_SSRX1:
			return DP_ML0;
		case USB_SSTX1:
			return DP_ML1;
		case USB_SSTX2:
			return DP_ML3;
		case USB_SSRX2:
			return DP_ML2;
		}
		break;
	}

	return -EINVAL;
}

/**
 * drm_dp_typec_bridge_assign_pins - Assign DisplayPort (DP) lanes to USB type-C pins
 * @typec_bridge_dev: Device created for the type-c bridge
 * @conf: DisplayPort altmode configure command VDO content
 * @orientation: Orientation of USB type-c port
 * @lane_mapping: Physical (array index) to logical (array value) USB type-C lane mapping
 *
 * Assign DP lanes to the USB type-C pins for the DP altmode configuration
 * @conf, while taking into account the USB type-C @lane_mapping. Future atomic
 * checks on this bridge will request the lane assignment from the previous
 * bridge so that the DP signal is sent to the proper USB type-C pins.
 *
 * Return: 0 on success, negative value for failure.
 */
int drm_dp_typec_bridge_assign_pins(struct drm_dp_typec_bridge_dev *typec_bridge_dev,
				     u32 conf, enum typec_orientation orientation,
				     enum usb_ss_lane lane_mapping[NUM_USB_SS])
{
	struct auxiliary_device *adev = &typec_bridge_dev->adev;
	struct drm_aux_hpd_bridge_data *hpd_data = auxiliary_get_drvdata(adev);
	struct drm_dp_typec_bridge_data *data;
	u8 *dp_lanes;
	size_t num_lanes, max_lanes;
	int i, typec_lane;
	u8 pin_assign;

	if (!hpd_data)
		return -EINVAL;

	data = hpd_bridge_to_typec_bridge_data(hpd_data);
	dp_lanes = data->dp_lanes;

	pin_assign = DP_CONF_GET_PIN_ASSIGN(conf);
	if (pin_assign == DP_PIN_ASSIGN_D)
		num_lanes = 2;
	else
		num_lanes = 4;
	max_lanes = typec_bridge_dev->max_lanes;
	data->num_lanes = num_lanes = min(num_lanes, max_lanes);

	for (i = 0; i < num_lanes; i++) {
		/* Get physical type-c lane for DP lane */
		typec_lane = dp_lane_to_typec_lane(i);
		if (typec_lane < 0) {
			dev_err(&adev->dev, "Invalid type-c lane configuration at DP_ML%d\n", i);
			return -EINVAL;
		}

		/* Map physical to logical type-c lane */
		typec_lane = lane_mapping[typec_lane];

		/* Map logical type-c lane to logical DP lane */
		dp_lanes[i] = typec_to_dp_lane(typec_lane, orientation);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(drm_dp_typec_bridge_assign_pins);

static int drm_dp_typec_bridge_atomic_check(struct drm_bridge *bridge,
					   struct drm_bridge_state *bridge_state,
					   struct drm_crtc_state *crtc_state,
					   struct drm_connector_state *conn_state)
{
	struct drm_dp_typec_bridge_data *data;
	struct drm_lane_cfg *in_lanes;
	u8 *dp_lanes;
	size_t num_lanes;
	int i;

	data = to_drm_dp_typec_bridge_data(bridge);
	num_lanes = data->num_lanes;
	if (!num_lanes)
		return 0;
	dp_lanes = data->dp_lanes;

	in_lanes = kcalloc(num_lanes, sizeof(*in_lanes), GFP_KERNEL);
	if (!in_lanes)
		return -ENOMEM;

	bridge_state->input_bus_cfg.lanes = in_lanes;
	bridge_state->input_bus_cfg.num_lanes = num_lanes;

	for (i = 0; i < num_lanes; i++)
		in_lanes[i].logical = dp_lanes[i];

	return 0;
}

static const struct drm_bridge_funcs drm_aux_hpd_bridge_funcs = {
	.attach	= drm_aux_hpd_bridge_attach,
};

static const struct drm_bridge_funcs drm_dp_typec_bridge_funcs = {
	.attach	= drm_aux_hpd_bridge_attach,
	.atomic_check = drm_dp_typec_bridge_atomic_check,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
};

enum drm_aux_bridge_type {
	DRM_AUX_HPD_BRIDGE,
	DRM_AUX_TYPEC_BRIDGE,
};

static int drm_aux_hpd_bridge_probe(struct auxiliary_device *auxdev,
				    const struct auxiliary_device_id *id)
{
	struct device *dev = &auxdev->dev;
	struct drm_aux_hpd_bridge_data *hpd_data;
	struct drm_dp_typec_bridge_dev *typec_bridge_dev;
	struct drm_dp_typec_bridge_data *typec_data;
	struct drm_bridge *bridge;
	u8 dp_lanes[] = { DP_ML0, DP_ML1, DP_ML2, DP_ML3 };

	if (id->driver_data == DRM_AUX_HPD_BRIDGE) {
		hpd_data = devm_kzalloc(dev, sizeof(*hpd_data), GFP_KERNEL);
		if (!hpd_data)
			return -ENOMEM;
		bridge = &hpd_data->bridge;
		bridge->funcs = &drm_aux_hpd_bridge_funcs;
		bridge->ops = DRM_BRIDGE_OP_HPD;
	} else if (id->driver_data == DRM_AUX_TYPEC_BRIDGE) {
		typec_data = devm_kzalloc(dev, sizeof(*typec_data), GFP_KERNEL);
		if (!typec_data)
			return -ENOMEM;
		hpd_data = &typec_data->hpd_bridge;
		bridge = &hpd_data->bridge;
		bridge->funcs = &drm_dp_typec_bridge_funcs;
		typec_bridge_dev = to_drm_dp_typec_bridge_dev(dev);
		if (!typec_bridge_dev->no_hpd)
			bridge->ops = DRM_BRIDGE_OP_HPD;
		hpd_data->no_hpd = typec_bridge_dev->no_hpd;
		memcpy(typec_data->dp_lanes, dp_lanes, sizeof(typec_data->dp_lanes));
	} else {
		return -ENODEV;
	}

	hpd_data->dev = dev;
	bridge->of_node = dev_get_platdata(dev);
	bridge->type = DRM_MODE_CONNECTOR_DisplayPort;

	auxiliary_set_drvdata(auxdev, hpd_data);

	return devm_drm_bridge_add(dev, bridge);
}

static const struct auxiliary_device_id drm_aux_hpd_bridge_table[] = {
	{ .name = KBUILD_MODNAME ".dp_hpd_bridge", .driver_data = DRM_AUX_HPD_BRIDGE, },
	{ .name = KBUILD_MODNAME ".dp_typec_bridge", .driver_data = DRM_AUX_TYPEC_BRIDGE, },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, drm_aux_hpd_bridge_table);

static struct auxiliary_driver drm_aux_hpd_bridge_drv = {
	.name = "aux_hpd_bridge",
	.id_table = drm_aux_hpd_bridge_table,
	.probe = drm_aux_hpd_bridge_probe,
};
module_auxiliary_driver(drm_aux_hpd_bridge_drv);

MODULE_AUTHOR("Dmitry Baryshkov <dmitry.baryshkov@linaro.org>");
MODULE_DESCRIPTION("DRM HPD bridge");
MODULE_LICENSE("GPL");
