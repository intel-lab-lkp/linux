// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025-2026, ITE. All Rights Reserved
 */
#include <linux/auxiliary_bus.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/usb/typec_retimer.h>

#include <drm/drm_bridge.h>

#include "itepd.h"

struct itepd_altmode;

struct itepd_altmode_port {
	struct itepd_altmode *altmode;
	unsigned int index;

	struct workqueue_struct *ordered_wq;

	struct typec_switch *typec_switch;
	struct typec_mux *typec_mux;
	struct typec_retimer *typec_retimer;
	struct drm_bridge bridge;

	enum typec_orientation orientation;
};

struct itepd_altmode {
	struct device *dev;

	struct itepd_altmode_port ports[ITEPD_MAX_PORTS];
};

struct itepd_altmode_work {
	struct itepd_altmode_port *alt_port;
	struct itepd_altmode_data data;
	struct work_struct work;

	/*
	 * DP mode state buffers kept on the heap (inside this kmalloc'd work
	 * item) so that itepd_altmode_dp() does not need large local variables
	 * and avoids triggering CONFIG_FRAME_WARN / checkstack.
	 */
	struct typec_altmode		dp_alt;
	struct typec_displayport_data	dp_data;
	struct typec_mux_state		mux_state;
	struct typec_retimer_state	retimer_state;
};

static enum typec_orientation itepd_altmode_mux_to_orientation(u8 mux)
{
	if (mux >= ITEPD_USBPD_MUX_OFF)
		return TYPEC_ORIENTATION_NONE;
	else
		return (mux & ITEPD_USBPD_MUX_FLIPPED) ?
			TYPEC_ORIENTATION_REVERSE : TYPEC_ORIENTATION_NORMAL;
}

static void itepd_altmode_safe(struct itepd_altmode_port *alt_port,
			       struct itepd_altmode_work *worker)
{
	struct itepd_altmode *altmode = alt_port->altmode;
	struct typec_mux_state mux_state = {};
	struct typec_retimer_state retimer_state = {};
	int ret;

	mux_state.alt = NULL;
	mux_state.data = NULL;
	mux_state.mode = TYPEC_STATE_SAFE;

	ret = typec_mux_set(alt_port->typec_mux, &mux_state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to safe mode\n");

	retimer_state.alt = NULL;
	retimer_state.data = NULL;
	retimer_state.mode = TYPEC_STATE_SAFE;

	ret = typec_retimer_set(alt_port->typec_retimer, &retimer_state);
	if (ret)
		dev_err(altmode->dev, "failed to setup retimer to safe mode\n");
}

static void itepd_altmode_usb(struct itepd_altmode_port *alt_port,
			      struct itepd_altmode_work *worker)
{
	struct itepd_altmode *altmode = alt_port->altmode;
	struct typec_mux_state mux_state = {};
	struct typec_retimer_state retimer_state = {};
	int ret;

	mux_state.alt = NULL;
	mux_state.data = NULL;
	mux_state.mode = TYPEC_STATE_USB;

	ret = typec_mux_set(alt_port->typec_mux, &mux_state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to USB\n");

	retimer_state.alt = NULL;
	retimer_state.data = NULL;
	retimer_state.mode = TYPEC_STATE_USB;

	ret = typec_retimer_set(alt_port->typec_retimer, &retimer_state);
	if (ret)
		dev_err(altmode->dev, "failed to setup retimer to USB\n");
}

static void itepd_altmode_dp(struct itepd_altmode_port *alt_port,
			     struct itepd_altmode_work *worker)
{
	struct itepd_altmode *altmode = alt_port->altmode;
	u32 pin_assign;
	unsigned int mode;
	int ret;

	/* Use heap buffers in worker to avoid large stack frames. */
	memset(&worker->dp_alt, 0, sizeof(worker->dp_alt));
	memset(&worker->dp_data, 0, sizeof(worker->dp_data));
	memset(&worker->mux_state, 0, sizeof(worker->mux_state));
	memset(&worker->retimer_state, 0, sizeof(worker->retimer_state));

	worker->dp_alt.svid   = USB_TYPEC_DP_SID;
	worker->dp_alt.mode   = USB_TYPEC_DP_MODE;
	worker->dp_alt.active = 1;

	worker->dp_data.status = worker->data.dp_status &
				 (DP_STATUS_ENABLED | DP_STATUS_HPD_STATE |
				  DP_STATUS_IRQ_HPD);
	worker->dp_data.conf = worker->data.dp_config & DP_CONF_PIN_ASSIGNEMENT_MASK;

	pin_assign = DP_CONF_GET_PIN_ASSIGN(worker->data.dp_config);
	if (pin_assign == BIT(DP_PIN_ASSIGN_C))
		mode = DP_PIN_ASSIGN_C;
	else if (pin_assign == BIT(DP_PIN_ASSIGN_D))
		mode = DP_PIN_ASSIGN_D;
	else
		mode = 0; /* unknown pin assignment — fall back to safe */

	worker->mux_state.alt  = &worker->dp_alt;
	worker->mux_state.data = &worker->dp_data;
	worker->mux_state.mode = mode ? TYPEC_MODAL_STATE(mode) : TYPEC_STATE_SAFE;

	ret = typec_mux_set(alt_port->typec_mux, &worker->mux_state);
	if (ret)
		dev_err(altmode->dev, "failed to switch mux to DP\n");

	worker->retimer_state.alt  = &worker->dp_alt;
	worker->retimer_state.data = &worker->dp_data;
	worker->retimer_state.mode = worker->mux_state.mode;

	ret = typec_retimer_set(alt_port->typec_retimer, &worker->retimer_state);
	if (ret)
		dev_err(altmode->dev, "failed to setup retimer to DP\n");
}

static void itepd_altmode_worker(struct work_struct *work)
{
	struct itepd_altmode_work *worker =
		container_of(work, struct itepd_altmode_work, work);
	struct itepd_altmode_port *alt_port = worker->alt_port;

	alt_port->orientation = itepd_altmode_mux_to_orientation(worker->data.mux);
	typec_switch_set(alt_port->typec_switch, alt_port->orientation);

	switch (worker->data.mux) {
	case ITEPD_USBPD_MUX_OFF:
		itepd_altmode_safe(alt_port, worker);
		drm_bridge_hpd_notify(&alt_port->bridge,
				      connector_status_disconnected);
		break;

	case ITEPD_USBPD_MUX_DP_0:
	case ITEPD_USBPD_MUX_DP_1:
	case ITEPD_USBPD_MUX_USB_DP_0:
	case ITEPD_USBPD_MUX_USB_DP_1:
		itepd_altmode_dp(alt_port, worker);
		if (worker->data.dp_status & DP_STATUS_HPD_STATE)
			drm_bridge_hpd_notify(&alt_port->bridge,
					      connector_status_connected);
		else
			drm_bridge_hpd_notify(&alt_port->bridge,
					      connector_status_disconnected);
		break;

	case ITEPD_USBPD_MUX_USB_0:
	case ITEPD_USBPD_MUX_USB_1:
	case ITEPD_USBPD_MUX_TBT_0:
	case ITEPD_USBPD_MUX_TBT_1:
	case ITEPD_USBPD_MUX_USB4_0:
	case ITEPD_USBPD_MUX_USB4_1:
		itepd_altmode_usb(alt_port, worker);
		drm_bridge_hpd_notify(&alt_port->bridge,
				      connector_status_disconnected);
		break;

	default:
		dev_err(alt_port->altmode->dev,
			"unknown mux state %u on port %u, forcing safe mode\n",
			worker->data.mux, alt_port->index);
		itepd_altmode_safe(alt_port, worker);
		drm_bridge_hpd_notify(&alt_port->bridge,
				      connector_status_disconnected);
		break;
	}

	kfree(worker);
}

static int itepd_altmode_attach(struct drm_bridge *bridge,
				struct drm_encoder *encoder,
				enum drm_bridge_attach_flags flags)
{
	return flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR ? 0 : -EINVAL;
}

static const struct drm_bridge_funcs itepd_altmode_bridge_funcs = {
	.attach = itepd_altmode_attach,
};

static void itepd_altmode_put_retimer(void *data)
{
	typec_retimer_put(data);
}

static void itepd_altmode_put_mux(void *data)
{
	typec_mux_put(data);
}

static void itepd_altmode_put_switch(void *data)
{
	typec_switch_put(data);
}

static void itepd_altmode_notify(void *priv, struct itepd_altmode_data *data)
{
	struct itepd_altmode *altmode = priv;
	struct itepd_altmode_port *alt_port;
	struct itepd_altmode_work *worker;

	if (data->port >= ARRAY_SIZE(altmode->ports)) {
		dev_err(altmode->dev, "invalid connector number, skip notify\n");
		return;
	}

	alt_port = &altmode->ports[data->port];
	if (!alt_port->altmode)
		return;

	worker = kmalloc_obj(*worker, GFP_KERNEL);
	if (!worker) {
		dev_err(altmode->dev, "out of memory, skip notify\n");
		return;
	}

	memcpy(&worker->data, data, sizeof(struct itepd_altmode_data));
	worker->alt_port = alt_port;

	INIT_WORK(&worker->work, itepd_altmode_worker);
	queue_work(alt_port->ordered_wq, &worker->work);
}

static void itepd_altmode_destroy_wq(void *data)
{
	struct workqueue_struct *wq = data;

	flush_workqueue(wq);
	destroy_workqueue(wq);
}

static int itepd_altmode_probe(struct auxiliary_device *adev,
			       const struct auxiliary_device_id *id)
{
	struct itepd_altmode *altmode;
	struct itepd_altmode_port *alt_port;
	struct itepd_altmode_cb *cb;
	struct fwnode_handle *fwnode;
	struct device *dev = &adev->dev;
	u32 port;
	int ret;

	altmode = devm_kzalloc(dev, sizeof(*altmode), GFP_KERNEL);
	if (!altmode)
		return -ENOMEM;

	cb = devm_kzalloc(dev, sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	altmode->dev = dev;

	device_for_each_child_node(dev, fwnode) {
		ret = fwnode_property_read_u32(fwnode, "reg", &port);
		if (ret < 0) {
			dev_err(dev, "missing reg property of %pOFn\n", fwnode);
			fwnode_handle_put(fwnode);
			return ret;
		}

		if (port >= ARRAY_SIZE(altmode->ports)) {
			dev_warn(dev, "invalid connector number, ignoring\n");
			continue;
		}

		if (altmode->ports[port].altmode) {
			dev_err(dev, "multiple connector definition for port %u\n", port);
			fwnode_handle_put(fwnode);
			return -EINVAL;
		}

		alt_port = &altmode->ports[port];
		alt_port->altmode = altmode;
		alt_port->index = port;

		alt_port->ordered_wq = alloc_ordered_workqueue("itepd_altmode_%u", 0, port);
		if (!alt_port->ordered_wq) {
			fwnode_handle_put(fwnode);
			return -ENOMEM;
		}

		ret = devm_add_action_or_reset(dev, itepd_altmode_destroy_wq,
					       alt_port->ordered_wq);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		alt_port->bridge.funcs = &itepd_altmode_bridge_funcs;
		alt_port->bridge.of_node = to_of_node(fwnode);
		alt_port->bridge.ops = DRM_BRIDGE_OP_HPD;
		alt_port->bridge.type = DRM_MODE_CONNECTOR_DisplayPort;

		alt_port->typec_mux = fwnode_typec_mux_get(fwnode);
		if (IS_ERR(alt_port->typec_mux)) {
			fwnode_handle_put(fwnode);
			return dev_err_probe(dev, PTR_ERR(alt_port->typec_mux),
					     "failed to acquire mode-switch for port: %d\n",
					     port);
		}

		ret = devm_add_action_or_reset(dev, itepd_altmode_put_mux,
					       alt_port->typec_mux);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		alt_port->typec_retimer = fwnode_typec_retimer_get(fwnode);
		if (IS_ERR(alt_port->typec_retimer)) {
			fwnode_handle_put(fwnode);
			return dev_err_probe(dev, PTR_ERR(alt_port->typec_retimer),
					     "failed to acquire retimer-switch for port: %d\n",
					     port);
		}

		ret = devm_add_action_or_reset(dev, itepd_altmode_put_retimer,
					       alt_port->typec_retimer);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}

		alt_port->typec_switch = fwnode_typec_switch_get(fwnode);
		if (IS_ERR(alt_port->typec_switch)) {
			fwnode_handle_put(fwnode);
			return dev_err_probe(dev, PTR_ERR(alt_port->typec_switch),
					     "failed to acquire orientation-switch for port: %d\n",
					     port);
		}

		ret = devm_add_action_or_reset(dev, itepd_altmode_put_switch,
					       alt_port->typec_switch);
		if (ret) {
			fwnode_handle_put(fwnode);
			return ret;
		}
	}

	for (port = 0; port < ARRAY_SIZE(altmode->ports); port++) {
		alt_port = &altmode->ports[port];
		if (!alt_port->altmode)
			continue;

		ret = devm_drm_bridge_add(dev, &alt_port->bridge);
		if (ret)
			return ret;
	}

	dev_set_drvdata(dev, altmode);

	cb->notify = itepd_altmode_notify;
	cb->priv = altmode;

	ret = itepd_register_cb(dev, ITEPD_CLIENT_ALTMODE, cb);
	if (ret)
		return ret;

	return 0;
}

static void itepd_altmode_remove(struct auxiliary_device *adev)
{
	itepd_register_cb(&adev->dev, ITEPD_CLIENT_ALTMODE, NULL);
	/*
	 * devm unwind handles workqueue flush/destroy and typec resource
	 * release in reverse probe order.
	 */
}

static const struct auxiliary_device_id itepd_altmode_id_table[] = {
	{ .name = "itepd.altmode", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, itepd_altmode_id_table);

static struct auxiliary_driver itepd_altmode_driver = {
	.name = "itepd_altmode",
	.probe = itepd_altmode_probe,
	.remove = itepd_altmode_remove,
	.id_table = itepd_altmode_id_table,
};

module_auxiliary_driver(itepd_altmode_driver);

MODULE_AUTHOR("Jeson Yang <jeson.yang@ite.com.tw>");
MODULE_DESCRIPTION("USB Type-C alternate mode driver for ITE Type-C PD controller");
MODULE_LICENSE("GPL");
