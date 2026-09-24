// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mutex.h>
#include "controller.h"
#include "otg.h"
#include "vq_common.h"

int otg_init(struct virtio_usb *vusb)
{
	struct virtio_usb_otg *otg =
		devm_kzalloc(&vusb->vdev->dev, sizeof(*otg), GFP_KERNEL);
	unsigned int i;

	if (!otg)
		return -ENOMEM;

	otg->vusb = vusb;
	vusb->otg = otg;
	for (i = 0; i < VIRTIO_USB_VQ_OTG_MAX; i++)
		otg->oqs[i] = &vusb->vqueues[vusb->otg_vq_base + i];

	mutex_init(&otg->lock);
	init_completion(&otg->completion);

	return 0;
}

void otg_deinit(struct virtio_usb *vusb)
{
	struct virtio_usb_otg *otg = vusb->otg;

	if (!otg)
		return;

	/* Wake potential OTG command waiters before releasing OTG objects. */
	complete_all(&otg->completion);

	vusb->otg = NULL;
}

/* Send an OTG command and get a response.
 *
 * The function is implemented as synchronous. Design pattern is
 * virtio_can.c/virtio_can_send_ctrl_msg()
 */
u32 otg_get_role(struct virtio_usb *vusb, int port_id, u32 *role)
{
	struct scatterlist sg_out, sg_in, *sgs[2] = { &sg_out, &sg_in };
	struct virtqueue *vq =
		vusb->otg->oqs[VIRTIO_USB_VQ_COMMAND_IDX]->vqueue;
	unsigned int len;
	u32 status = VIRTIO_USB_S_ERR_INTERNAL;

	struct otg_get_role {
		struct virtio_usb_otg_cmd_hdr cmd_hdr;
		struct virtio_usb_otg_cmd_role cmd_role;
	} *msg = kzalloc(sizeof(struct otg_get_role), GFP_KERNEL);

	if (!msg)
		return status;

	msg->cmd_hdr.code = cpu_to_le32(VIRTIO_USB_CMD_OTG_GET_ROLE);
	msg->cmd_hdr.port = cpu_to_le32(port_id);
	sg_init_one(&sg_out, &msg->cmd_hdr, sizeof(msg->cmd_hdr));
	sg_init_one(&sg_in, &msg->cmd_role, sizeof(msg->cmd_role));

	mutex_lock(&vusb->otg->lock);

	if (virtqueue_add_sgs(vq, sgs, 1u, 1u, msg, GFP_ATOMIC)) {
		pr_err("%s virtqueue_add_sgs error\n", __func__);
		goto exit;
	}

	if (!virtqueue_kick(vq)) {
		pr_err("%s virtqueue_kick error\n", __func__);
		goto exit;
	}

	while (!virtqueue_get_buf(vq, &len) && !virtqueue_is_broken(vq))
		wait_for_completion(&vusb->otg->completion);

	status = le32_to_cpu(msg->cmd_role.status.code);
	*role = le32_to_cpu(msg->cmd_role.role);

	if (*role != VIRTIO_USB_ROLE_HOST && *role != VIRTIO_USB_ROLE_DEVICE)
		pr_err("%s - wrong role (%d)\n", __func__, *role);
	else {
		pr_info("%s otg_role %s\n", __func__,
			*role == VIRTIO_USB_ROLE_HOST ?
				"VIRTIO_USB_ROLE_HOST" :
				"VIRTIO_USB_ROLE_DEVICE");
	}

exit:
	kfree(msg);
	mutex_unlock(&vusb->otg->lock);
	return status;
}

static void virtio_usb_otg_cmd_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;

	if (!vusb->otg)
		return;

	complete(&vusb->otg->completion);
}

static void virtio_usb_otg_cmdq_stop_cb(struct virtio_usb *vusb,
					struct virtio_usb_queue *vq)
{
	unsigned long flags;

	if (!vusb->otg || !vq->vqueue)
		return;

	/*
	 * Wake sleepers in OTG synchronous command paths so they can
	 * observe started=false and exit.
	 */
	complete_all(&vusb->otg->completion);

	spin_lock_irqsave(&vq->lock, flags);
	virtqueue_disable_cb(vq->vqueue);
	spin_unlock_irqrestore(&vq->lock, flags);
}

static void virtio_usb_otg_evtq_stop_cb(struct virtio_usb *vusb,
					struct virtio_usb_queue *vq)
{
	unsigned long flags;
	u32 length;
	void *buf;

	if (!vq->vqueue)
		return;

	/* The OTG event queue is not populated yet at this stage (no
	 * VIRTIO_USB_F_SWITCH_ROLE negotiation, no CHANGE_ROLE events),
	 * so this only has to make sure del_vqs() finds the ring empty.
	 */
	spin_lock_irqsave(&vq->lock, flags);
	virtqueue_disable_cb(vq->vqueue);
	while ((buf = virtqueue_get_buf(vq->vqueue, &length)))
		;
	spin_unlock_irqrestore(&vq->lock, flags);
}

const struct virtio_usb_vq_desc otg_vqueues[VIRTIO_USB_VQ_OTG_MAX] = {
	[VIRTIO_USB_VQ_COMMAND_IDX] = {
		.callback = virtio_usb_otg_cmd_notify_cb,
		.name = "virtusb-otg-cmd",
		.process = NULL,
		.stop = virtio_usb_otg_cmdq_stop_cb,
	},
	[VIRTIO_USB_VQ_EVENT_IDX] = {
		.callback = NULL,
		.name = "virtusb-otg-evt",
		.process = NULL,
		.stop = virtio_usb_otg_evtq_stop_cb,
	},
};
