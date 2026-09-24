// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mutex.h>
#include "controller.h"
#include "host.h"
#include "otg.h"
#include "vq_common.h"

// for usb_role enum from include/linux/usb/role.h
const char *role_switch_role_names[] = { "USB_ROLE_NONE", "USB_ROLE_HOST",
					 "USB_ROLE_DEVICE" };

static int vusb_set_role_cb(struct usb_role_switch *sw, enum usb_role role)
{
	struct virtio_usb_port *vport = usb_role_switch_get_drvdata(sw);

	pr_info("%s set_role to %s\n", __func__, role_switch_role_names[role]);
	mutex_lock(&vport->otg->lock);

	if (role == USB_ROLE_HOST && vport->role == VIRTIO_USB_ROLE_DEVICE) {
		pr_warn("%s we are in device role, switch to host\n", __func__);
		schedule_work(&vport->otg->set_role_work);
	}
	mutex_unlock(&vport->otg->lock);
	return 0;
}

static enum usb_role vusb_get_role_cb(struct usb_role_switch *sw)
{
	struct virtio_usb_port *vport = usb_role_switch_get_drvdata(sw);

	switch (vport->role) {
	case VIRTIO_USB_ROLE_HOST:
		return USB_ROLE_HOST;
	case VIRTIO_USB_ROLE_DEVICE:
		return USB_ROLE_DEVICE;
	default:
		return USB_ROLE_NONE;
	}
}

static void virtio_usb_otg_event_process(struct virtio_usb_event *event)
{
	struct virtio_usb *vusb = event->vusb;
	struct virtio_usb_otg_event *evt = virtio_usb_event_buf(event);
	int port_id = le32_to_cpu(evt->port);
	struct virtio_usb_port *vport = &vusb->vports[port_id];
	u32 status, role, supported_role, old_role;
	int rc;

	if (le32_to_cpu(evt->code) != VIRTIO_USB_EVT_OTG_CHANGE_ROLE) {
		pr_err("%s wrong event code %d\n", __func__,
		       le32_to_cpu(evt->code));
		return;
	}

	old_role = vport->role;
	pr_info("%s VIRTIO_USB_EVT_OTG_CHANGE_ROLE curr_role %s\n", __func__,
		old_role == VIRTIO_USB_ROLE_DEVICE ? "VIRTIO_USB_ROLE_DEVICE" :
						     "VIRTIO_USB_ROLE_HOST");

	status = otg_get_role(vusb, port_id, &role, &supported_role);
	if (status != VIRTIO_USB_S_OK) {
		pr_err("%s otg_get_role status %d\n", __func__, status);
		return;
	}

	/* A port's usb_hcd/root hub is only supposed to exist while that
	 * port is actually in host role - it's immediately visible to the
	 * rest of the kernel (lsusb, udev, ...) the moment usb_add_hcd()
	 * runs, unlike the device-role side's vudc, which stays dormant
	 * and kernel-invisible until a later BIND event. So, unlike vudc
	 * (already unconditionally pre-allocated for every OTG-capable
	 * port at probe time, regardless of its current role - see
	 * virtio_usb_probe()), vhc must be created/destroyed dynamically,
	 * exactly at the moment a port's role actually changes.
	 */
	if (role != old_role && role == VIRTIO_USB_ROLE_HOST) {
		rc = virtio_usb_hc_vp_init(vusb, port_id);
		if (rc) {
			pr_err("%s virtio_usb_hc_vp_init() port=%d error(%d)\n",
			       __func__, port_id, rc);
			/* Leave vport->role at its old value: reporting a
			 * host role with no working HCD behind it would be
			 * worse than not switching at all.
			 */
			return;
		}
	} else if (role != old_role && old_role == VIRTIO_USB_ROLE_HOST) {
		virtio_usb_hc_vp_deinit(vusb, port_id);
	}

	vport->role = role;
	vport->is_otg = supported_role & (1UL << VIRTIO_USB_F_SWITCH_ROLE);
	pr_info("%s The new role (%s) is set\n", __func__,
		role == VIRTIO_USB_ROLE_DEVICE ? "VIRTIO_USB_ROLE_DEVICE" :
						 "VIRTIO_USB_ROLE_HOST");
}

/**
 * virtio_usb_otg_event_work() - OTG event queue receive worker.
 * @work: Kernel work to handle event completion.
 *
 * Context: Process context.
 */
static void virtio_usb_otg_event_work(struct work_struct *work)
{
	struct virtio_usb_otg *otg =
		container_of(work, struct virtio_usb_otg, event_process_work);
	struct virtio_usb_queue *evtq = otg->oqs[VIRTIO_USB_VQ_EVENT_IDX];

	virtio_usb_evt_work(evtq, virtio_usb_otg_event_process);
}

/**
 * virtio_usb_otg_set_role_work() - OTG send command to device worker.
 *
 * Context: Process context.
 */
static void virtio_usb_otg_set_role_work(struct work_struct *work)
{
	struct virtio_usb_port_otg *vport_otg =
		container_of(work, struct virtio_usb_port_otg, set_role_work);
	struct virtio_usb_otg *otg = vport_otg->vusb->otg;
	struct virtio_usb_otg_cmd_hdr *hdr;
	struct virtio_usb_cmd *cmd;
	int rc;

	cmd = virtio_usb_cmd_alloc(
		sizeof(*hdr), sizeof(struct virtio_usb_cmd_status), GFP_KERNEL);
	if (!cmd)
		return;

	hdr = virtio_usb_cmd_request(cmd);
	hdr->code = cpu_to_le32(VIRTIO_USB_CMD_OTG_SWITCH_ROLE);
	hdr->port = cpu_to_le32(vport_otg->port_id);
	cmd->msg.queue = otg->oqs[VIRTIO_USB_VQ_COMMAND_IDX];

	mutex_lock(&otg->lock);
	rc = virtio_usb_cmd_send_sync(vport_otg->vusb, NULL, NULL, cmd);
	mutex_unlock(&otg->lock);

	if (rc)
		pr_err("%s virtio_usb_cmd_send_sync() error %d\n", __func__,
		       rc);
	else
		pr_info("%s success\n", __func__);
}

/**
 * virtio_usb_otg_event_populate() - Add events to the otg event queue.
 * @vusb: VirtIO USB device.
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure.
 */
static int virtio_usb_otg_event_populate(struct virtio_usb *vusb)
{
	struct virtio_usb_queue *otg_evt_queue =
		vusb->otg->oqs[VIRTIO_USB_VQ_EVENT_IDX];
	struct virtio_usb_event *events;

	events = virtio_usb_events_alloc(vusb, otg_evt_queue,
					 sizeof(struct virtio_usb_otg_event));
	if (!events)
		return -ENOMEM;

	return virtio_usb_events_populate(events);
}

static void otg_vports_deinit(struct virtio_usb *vusb)
{
	if (!vusb->vports)
		return;

	for (int i = 0; i < vusb->nports; ++i) {
		struct virtio_usb_port *vport = &vusb->vports[i];
		/* is_otg is set before otg_sysfs_init() runs (which is
		 * where vport->otg actually gets allocated) - a probe
		 * failure in between the two would otherwise leave
		 * is_otg true but otg still NULL here.
		 */
		if (!vport->is_otg || !vport->otg)
			continue;

		cancel_work_sync(&vport->otg->set_role_work);
		usb_role_switch_unregister(vport->otg->sw);
		vport->otg->sw = NULL;
		kfree(vport->otg->sysfs_name);
		vport->otg->sysfs_name = NULL;
	}
}

static int otg_vport_init(struct virtio_usb *vusb, int port_id)
{
	struct device *dev = &vusb->vdev->dev;
	struct virtio_usb_port *vport = &vusb->vports[port_id];
	struct virtio_usb_port_otg *vport_otg;
	struct usb_role_switch_desc desc = { 0 };

	vport_otg = devm_kzalloc(dev, sizeof(*vport_otg), GFP_KERNEL);
	if (!vport_otg)
		return -ENOMEM;

	vport_otg->vusb = vusb;
	vport_otg->port_id = port_id;
	vport_otg->sysfs_name = kasprintf(GFP_KERNEL, "port%u", port_id);

	mutex_init(&vport_otg->lock);
	INIT_WORK(&vport_otg->set_role_work, virtio_usb_otg_set_role_work);

	desc.name = vport_otg->sysfs_name;
	desc.set = vusb_set_role_cb;
	desc.get = vusb_get_role_cb;
	desc.driver_data = vport;
	desc.fwnode = NULL;
	desc.allow_userspace_control = true;

	vport_otg->sw = usb_role_switch_register(dev, &desc);
	if (IS_ERR(vport_otg->sw)) {
		int rc = PTR_ERR(vport_otg->sw);
		dev_err(dev, "port%u: usb_role_switch_register() failed: %d\n",
			port_id, rc);
		vport_otg->sw = NULL;
		kfree(vport_otg->sysfs_name);
		vport_otg->sysfs_name = NULL;
		return rc;
	}

	vport->otg = vport_otg;
	return 0;
}

int otg_init(struct virtio_usb *vusb)
{
	int rc;
	struct virtio_usb_otg *otg =
		devm_kzalloc(&vusb->vdev->dev, sizeof(*otg), GFP_KERNEL);
	unsigned int i;

	if (!otg)
		return -ENOMEM;

	otg->vusb = vusb;
	vusb->otg = otg;
	for (i = 0; i < VIRTIO_USB_VQ_OTG_MAX; i++)
		otg->oqs[i] = &vusb->vqueues[vusb->otg_vq_base + i];

	INIT_WORK((struct work_struct *)&otg->event_process_work,
		  virtio_usb_otg_event_work);

	mutex_init(&otg->lock);

	rc = virtio_usb_otg_event_populate(vusb);
	if (rc)
		return rc;

	return 0;
}

int otg_sysfs_init(struct virtio_usb *vusb)
{
	int i, rc;

	for (i = 0; i < vusb->nports; i++)
		if (vusb->vports[i].is_otg) {
			/* Skip if already registered - probe may be retried
			 * after a Virtio reset and the role switch device
			 * persists across retries (devm allocation).
			 * Calling usb_role_switch_register() again would
			 * return -EEXIST from device_register().
			 */
			if (vusb->vports[i].otg)
				continue;
			rc = otg_vport_init(vusb, i);
			if (rc)
				return rc;
		}
	return 0;
}

void otg_deinit(struct virtio_usb *vusb)
{
	struct virtio_usb_otg *otg = vusb->otg;

	if (!otg)
		return;

	/*
	 * event_process_work is already cancelled by
	 * virtio_usb_otg_evtq_stop_cb(), called earlier in
	 * virtio_usb_remove()'s vqueues[i].stop() loop.
	 */
	otg_vports_deinit(vusb);
	vusb->otg = NULL;
}

/**
 * virtio_usb_otg_evt_notify_cb() - OTG Event virtqueue notification callback
 * @vqueue: Underlying event virtqueue.
 *
 * This callback function is called upon a vring interrupt request from the
 * device.
 *
 * Context: Interrupt context.
 */
static void virtio_usb_otg_evt_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;

	if (!READ_ONCE(vusb->started) || !vusb->otg)
		return;

	schedule_work(&vusb->otg->event_process_work);
}

/* Send an OTG command and get a response.
 *
 * The function is implemented as synchronous. Design pattern is
 * virtio_can.c/virtio_can_send_ctrl_msg()
 */
u32 otg_get_role(struct virtio_usb *vusb, int port_id, u32 *role,
		 u32 *supported_role)
{
	struct virtio_usb_otg_cmd_hdr *hdr;
	struct virtio_usb_otg_cmd_role *resp;
	struct virtio_usb_cmd *cmd;
	u32 status = VIRTIO_USB_S_ERR_INTERNAL;
	int rc;

	cmd = virtio_usb_cmd_alloc(sizeof(*hdr), sizeof(*resp), GFP_KERNEL);
	if (!cmd)
		return status;

	hdr = virtio_usb_cmd_request(cmd);
	hdr->code = cpu_to_le32(VIRTIO_USB_CMD_OTG_GET_ROLE);
	hdr->port = cpu_to_le32(port_id);
	cmd->msg.queue = vusb->otg->oqs[VIRTIO_USB_VQ_COMMAND_IDX];

	virtio_usb_cmd_ref(cmd);

	mutex_lock(&vusb->otg->lock);
	rc = virtio_usb_cmd_send_sync(vusb, NULL, NULL, cmd);
	mutex_unlock(&vusb->otg->lock);

	if (rc) {
		pr_err("%s virtio_usb_cmd_send_sync() error %d\n", __func__,
		       rc);
		virtio_usb_cmd_unref(cmd);
		return status;
	}

	resp = virtio_usb_cmd_response(cmd);
	status = le32_to_cpu(resp->status.code);
	*role = le32_to_cpu(resp->role);
	*supported_role = le32_to_cpu(resp->supported_role);
	virtio_usb_cmd_unref(cmd);

	if (*role != VIRTIO_USB_ROLE_HOST && *role != VIRTIO_USB_ROLE_DEVICE)
		pr_err("%s - wrong role (%d)\n", __func__, *role);
	else {
		pr_info("%s otg_role %s\n", __func__,
			*role == VIRTIO_USB_ROLE_HOST ?
				"VIRTIO_USB_ROLE_HOST" :
				"VIRTIO_USB_ROLE_DEVICE");
	}

	return status;
}

static void virtio_usb_otg_evtq_stop_cb(struct virtio_usb *vusb,
					struct virtio_usb_queue *vq)
{
	/*
	 * Now that otg_get_role()/virtio_usb_otg_set_role_work() go
	 * through the common, timeout-protected virtio_usb_cmd_send_sync()
	 * instead of blocking indefinitely on virtqueue_is_broken(), it's
	 * safe to cancel event_process_work directly here, same as every
	 * other role's own evtq stop callback.
	 */
	virtio_usb_evt_drain_stop_cb(
		vq, vusb->otg ? &vusb->otg->event_process_work : NULL);
}

const struct virtio_usb_vq_desc otg_vqueues[VIRTIO_USB_VQ_OTG_MAX] = {
	[VIRTIO_USB_VQ_COMMAND_IDX] = {
		.callback = virtio_usb_cmd_notify_cb,
		.name = "virtusb-otg-cmd",
		.process = virtio_usb_cmd_process_cb,
		.stop = virtio_usb_cmdq_stop_cb,
	},
	[VIRTIO_USB_VQ_EVENT_IDX] = {
		.callback = virtio_usb_otg_evt_notify_cb,
		.name = "virtusb-otg-evt",
		.process = NULL,
		.stop = virtio_usb_otg_evtq_stop_cb,
	},
};
