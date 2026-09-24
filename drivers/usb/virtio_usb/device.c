// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio_usb: VirtIO USB device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <uapi/linux/virtio_usb.h>

#include "controller.h"
#include "device.h"

#define GADGET_NAME "virtio_usb_dc"

/**
 * struct virtio_usb_dc_priv - Device controller data priv
 * Structure for UDC related data in data messages
 * @req: usb_request structure
 * @vep: virtio usb endpoint
 * @vreq: virtio-usb request
 */
struct virtio_usb_dc_priv {
	struct usb_request req;
	struct virtio_usb_ep *vep;
	struct virtio_usb_data *vreq;
};

/**
 * usb_ep_to_virtio_ep() - Get the virtio usb endpoint from usb endpoint
 * @ep: usb endpoint
 *
 * Context: Any context.
 * Return: Pointer to virtio_usb_ep
 */
static struct virtio_usb_ep *usb_ep_to_virtio_ep(struct usb_ep *ep)
{
	return container_of(ep, struct virtio_usb_ep, ep);
}

/**
 * usb_ep_dir_in() - check if the endpoint has IN direction
 * @ep: usb endpoint
 *
 * Context: Any context.
 * Return: 1 if direction is USB_DIR_IN else 0
 */
static unsigned int usb_ep_dir_in(struct usb_ep *ep)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	unsigned int direction = 0;

	if (ep->address == 0)
		direction = vep->setup.bRequestType & USB_DIR_IN ? 1 : 0;
	else if (ep->desc && ep->desc->bEndpointAddress)
		direction = usb_endpoint_dir_in(ep->desc) ? 1 : 0;

	return direction;
}

/**
 * usb_req_to_virtio_data() - Get the virtio usb data message from usb_request
 * @req: usb_request structure
 *
 * Context: Any context.
 * Return: virtio usb data message
 */
static struct virtio_usb_data *usb_req_to_virtio_data(struct usb_request *req)
{
	struct virtio_usb_dc_priv *priv =
		container_of(req, struct virtio_usb_dc_priv, req);

	return priv->vreq;
}

/**
 * virtio_usb_dc_complete_req() - Completes a usb request
 * @vreq: virtio usb data message.
 *
 * Context: Process context.
 */
static void virtio_usb_dc_complete_req(struct virtio_usb_data *vreq)
{
	struct virtio_usb_response *response = virtio_usb_data_response(vreq);
	struct virtio_usb_dc_priv *priv = virtio_usb_data_priv(vreq);
	unsigned int status = le32_to_cpu(response->status);
	struct usb_request *req = &priv->req;
	struct virtio_usb_ep *vep = priv->vep;
	struct virtio_usb_dc *vudc = vep->vudc;
	struct virtio_usb_data *vreq_iter;
	unsigned int found = 0;
	unsigned long flags;

	spin_lock_irqsave(&vudc->lock, flags);

	list_for_each_entry(vreq_iter, &vep->req_queue, list) {
		if (vreq_iter == vreq) {
			found = 1;
			break;
		}
	}
	if (!found) {
		spin_unlock_irqrestore(&vudc->lock, flags);
		return;
	}
	if (req->status != -ECONNRESET && req->status != -ESHUTDOWN) {
		req->status = virtio_error_to_usb(status);
		if (!req->status)
			req->actual = le32_to_cpu(response->actual_length);
	}

	list_del_init(&vreq->list);
	spin_unlock_irqrestore(&vudc->lock, flags);
	virtio_usb_data_unref(vreq);
	usb_gadget_giveback_request(&vep->ep, req);
}

/**
 * virtio_usb_dc_data_work() - Worker to get all completed data message
 * from the virtqueue and call the completion handler.
 * @work: kernel work to handle data message completion.
 *
 * Context: Process context.
 */
void virtio_usb_dc_data_work(struct work_struct *work)
{
	struct virtio_usb *vusb =
		container_of(work, struct virtio_usb, vq_dev_data_rx_work);
	struct virtio_usb_queue *dataq =
		&vusb->vqueues[vusb->dev_vq_base + VIRTIO_USB_VQ_DATA_IDX];
	struct virtio_usb_data *vreq;
	unsigned int length;

	spin_lock_irq(&dataq->lock);
	do {
		virtqueue_disable_cb(dataq->vqueue);
		while ((vreq = virtqueue_get_buf(dataq->vqueue, &length))) {
			spin_unlock_irq(&dataq->lock);
			virtio_usb_dc_complete_req(vreq);
			spin_lock_irq(&dataq->lock);
		}
		if (unlikely(virtqueue_is_broken(dataq->vqueue)))
			break;
	} while (!virtqueue_enable_cb(dataq->vqueue));
	spin_unlock_irq(&dataq->lock);
}

/**
 * virtio_usb_dc_data_alloc() - Allocate and initialize a device controller
 * data message.
 * @ep: VirtIO usb device
 * @gfp: Kernel flags for memory allocation.
 *
 * The message will be automatically freed when the ref_count value is 0.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated data message on success, NULL on failure.
 */
static struct virtio_usb_data *virtio_usb_dc_data_alloc(struct usb_ep *ep,
							gfp_t gfp)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_request *request;
	struct virtio_usb_response *response;
	size_t request_size = sizeof(*request);
	size_t response_size = sizeof(*response);
	struct virtio_usb_dc_priv *priv;
	struct virtio_usb_data *vreq;

	vreq = virtio_usb_data_alloc(request_size, response_size,
				     sizeof(struct virtio_usb_dc_priv), gfp);
	if (!vreq)
		return NULL;

	priv = virtio_usb_data_priv(vreq);
	priv->vep = vep;
	priv->vreq = vreq;
	vreq->msg.queue =
		&vep->vudc->vusb->vqueues[vep->vudc->vusb->dev_vq_base +
					  VIRTIO_USB_VQ_DATA_IDX];

	request = virtio_usb_data_request(vreq);

	request->tag = cpu_to_le64((uintptr_t)vreq);

	return vreq;
}

/**
 * virtio_usb_dc_cmd_alloc() - Allocate and initialize the device controller
 * command message.
 * @vusb: VirtIO usb device
 * @command: command message
 * @gfp: Kernel flags for memory allocation.
 *
 * The message will be automatically freed when the ref_count value is 0.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated command message on success, NULL on failure.
 */
static struct virtio_usb_cmd *
virtio_usb_dc_cmd_alloc(struct virtio_usb_dc *vudc, unsigned int command,
			gfp_t gfp)
{
	size_t request_size = sizeof(struct virtio_usb_dev_cmd_hdr);
	size_t response_size = sizeof(struct virtio_usb_cmd_status);
	struct virtio_usb_cmd_status *status;
	struct virtio_usb_dev_cmd_hdr *hdr;
	struct virtio_usb_cmd *cmd;

	switch (command) {
	case VIRTIO_USB_CMD_DEV_GET_ENDPOINT_COUNT:
		response_size = sizeof(struct virtio_usb_dev_ep_count);
		break;
	case VIRTIO_USB_CMD_DEV_GET_FRAME_NUMBER:
		response_size = sizeof(struct virtio_usb_dev_frame_number);
		break;
	case VIRTIO_USB_CMD_DEV_CANCEL:
		request_size = sizeof(struct virtio_usb_dev_cmd_cancel);
		break;
	case VIRTIO_USB_CMD_DEV_VBUS_DRAW:
	case VIRTIO_USB_CMD_DEV_PULLUP:
	case VIRTIO_USB_CMD_DEV_EP_SET_HALT:
	case VIRTIO_USB_CMD_DEV_SET_SELF_POWERED:
		request_size = sizeof(struct virtio_usb_dev_cmd_set_value);
		break;
	}

	cmd = virtio_usb_cmd_alloc(request_size, response_size, gfp);
	if (!cmd)
		return NULL;

	hdr = virtio_usb_cmd_request(cmd);
	status = virtio_usb_cmd_response(cmd);

	hdr->code = cpu_to_le32(command);
	hdr->port = cpu_to_le16(vudc->port);
	cmd->msg.queue = &vudc->vusb->vqueues[vudc->vusb->dev_vq_base +
					      VIRTIO_USB_VQ_COMMAND_IDX];
	status->code = cpu_to_le32(VIRTIO_USB_S_ERR_CANCELLED);
	return cmd;
}

/* Endpoint callbacks */

/**
 * virtio_ep_enable() - Enable endpoint
 * This callback is called to configure endpoint and make it usable.
 * It is called to enable all endpoints except ep0
 * @ep: endpoint object
 * @desc: endpoint descriptor
 *
 * Context: Any context.
 */
static int virtio_ep_enable(struct usb_ep *ep,
			    const struct usb_endpoint_descriptor *desc)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_dc *vudc = vep->vudc;
	struct scatterlist sg;
	struct scatterlist *psg_data = &sg;
	struct virtio_usb_dev_cmd_hdr *hdr;
	struct virtio_usb_cmd *cmd;
	unsigned long flags;
	u16 endpoint;
	int rc;

	if (!vudc->driver)
		return -ESHUTDOWN;

	spin_lock_irqsave(&vudc->lock, flags);

	if (!ep || !desc || ep->caps.type_control ||
	    desc->bDescriptorType != USB_DT_ENDPOINT) {
		spin_unlock_irqrestore(&vudc->lock, flags);
		return -EINVAL;
	}

	ep->desc = desc;
	ep->maxpacket = usb_endpoint_maxp(desc);
	endpoint = vep->ep_id | (usb_ep_dir_in(ep) ? VIRTIO_USB_EP_DIR_IN :
						     VIRTIO_USB_EP_DIR_OUT);

	spin_unlock_irqrestore(&vudc->lock, flags);

	cmd = virtio_usb_dc_cmd_alloc(vudc, VIRTIO_USB_CMD_DEV_EP_ENABLE,
				      GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	sg_init_one(psg_data, desc, sizeof(*desc));
	hdr = virtio_usb_cmd_request(cmd);
	hdr->endpoint = cpu_to_le16(endpoint);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, psg_data, NULL, cmd);

	return rc;
}

/**
 * virtio_ep_disable() - Disable endpoint
 * This callback is called to disable endpoint which was
 * enabled using ep_enable callback
 * @ep: endpoint object
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_ep_disable(struct usb_ep *ep)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_data *vreq = NULL, *vreq_tmp;
	struct virtio_usb_dc *vudc = vep->vudc;
	struct virtio_usb_dc_priv *priv = NULL;
	struct virtio_usb_dev_cmd_hdr *hdr;
	struct virtio_usb_cmd *cmd;
	unsigned long flags;
	u16 endpoint;
	int rc;

	if (!ep || ep->caps.type_control)
		return -EINVAL;

	spin_lock_irqsave(&vudc->lock, flags);

	list_for_each_entry_safe(vreq, vreq_tmp, &vep->req_queue, list) {
		priv = virtio_usb_data_priv(vreq);
		/**
		 * When endpoint is disabled, completion handler for all pending
		 * requests will be called. Make the request status to -ESHUTDOWN
		 * to prevent requests completes even before the endpoint disable
		 * is send to the controller.
		 */
		priv->req.status = -ESHUTDOWN;
	}
	endpoint = vep->ep_id | (usb_ep_dir_in(ep) ? VIRTIO_USB_EP_DIR_IN :
						     VIRTIO_USB_EP_DIR_OUT);
	spin_unlock_irqrestore(&vudc->lock, flags);

	cmd = virtio_usb_dc_cmd_alloc(vudc, VIRTIO_USB_CMD_DEV_EP_DISABLE,
				      GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	hdr = virtio_usb_cmd_request(cmd);
	hdr->endpoint = cpu_to_le16(endpoint);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);

	return rc;
}

/**
 * virtio_ep_alloc_request() - Allocates request
 * @ep: Endpoint object associated with request
 * @mem_flags: mem flags
 *
 * Return: allocated request address, NULL on allocation error
 */
static struct usb_request *virtio_ep_alloc_request(struct usb_ep *ep,
						   gfp_t mem_flags)
{
	struct virtio_usb_dc_priv *priv;
	struct virtio_usb_data *vreq;

	vreq = virtio_usb_dc_data_alloc(ep, mem_flags);
	if (!vreq)
		return NULL;

	priv = virtio_usb_data_priv(vreq);

	return &priv->req;
}

/**
 * virtio_ep_free_request() - Free memory occupied by request
 * @ep: Endpoint object associated with request
 * @req: Request to be freed
 */
static void virtio_ep_free_request(struct usb_ep *ep, struct usb_request *req)
{
	struct virtio_usb_data *vreq = usb_req_to_virtio_data(req);

	virtio_usb_data_unref(vreq);
}

/**
 * virtio_ep_queue() - Transfer data on and endpoint
 * @ep: Pointer to endpoint object
 * @req: Pointer to request object
 * @mem_flags: gfp flags
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_ep_queue(struct usb_ep *ep, struct usb_request *req,
			   gfp_t mem_flags)
{
	struct scatterlist *out_sgs = NULL, *in_sgs = NULL, *psg_data;
	struct virtio_usb_data *vreq = usb_req_to_virtio_data(req);
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_dc *vudc = vep->vudc;
	struct virtio_usb_response *response;
	struct virtio_usb_request *request;
	u16 transfer_flags = 0;
	struct scatterlist sg;
	unsigned long flags;
	u16 endpoint;
	int rc;

	virtio_usb_data_ref(vreq);

	spin_lock_irqsave(&vudc->lock, flags);
	req->actual = 0;
	req->status = -EINPROGRESS;

	list_add_tail(&vreq->list, &vep->req_queue);
	endpoint = vep->ep_id | (usb_ep_dir_in(ep) ? VIRTIO_USB_EP_DIR_IN :
						     VIRTIO_USB_EP_DIR_OUT);
	spin_unlock_irqrestore(&vudc->lock, flags);

	request = virtio_usb_data_request(vreq);
	response = virtio_usb_data_response(vreq);

	response->actual_length = cpu_to_le32(0);
	response->status = cpu_to_le32(VIRTIO_USB_S_ERR_INTERNAL);

	if (req->short_not_ok)
		transfer_flags |= VIRTIO_USB_FLAG_SHORT_NOT_OK;
	else if (req->zero)
		transfer_flags |= VIRTIO_USB_FLAG_ZERO_PACKET;

	request->transfer_flags = cpu_to_le16(transfer_flags);
	request->endpoint = cpu_to_le16(endpoint);
	request->port = cpu_to_le16(vudc->port);

	if (req->length && req->buf) {
		psg_data = &sg;
		sg_init_one(psg_data, req->buf, req->length);
	} else if (req->length && req->num_sgs > 0) {
		psg_data = req->sg;
	} else if (req->sg) {
		psg_data = &sg;
		sg_init_one(psg_data, sg_virt(req->sg), req->length);
	} else {
		psg_data = NULL;
	}

	if (usb_ep_dir_in(ep))
		out_sgs = psg_data;
	else
		in_sgs = psg_data;

	rc = virtio_usb_data_send(vudc->vusb, vreq, out_sgs, in_sgs);
	if (rc)
		goto on_error_vq;

	return rc;

on_error_vq:
	spin_lock_irqsave(&vudc->lock, flags);
	list_del_init(&vreq->list);
	spin_unlock_irqrestore(&vudc->lock, flags);

	virtio_usb_data_unref(vreq);

	return rc;
}

/**
 * virtio_ep_dequeue() - Remove request from transfer queue
 * @ep: Endpoint object associated with request
 * @req: Request object
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_ep_dequeue(struct usb_ep *ep, struct usb_request *req)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_dev_cmd_cancel *cancel;
	struct virtio_usb_dc *vudc = vep->vudc;
	struct virtio_usb_dc_priv *priv;
	struct virtio_usb_data *vreq;
	struct virtio_usb_cmd *cmd;
	unsigned long flags;
	int rc = -EINVAL;
	u16 endpoint;

	if (!vudc->driver)
		return -ESHUTDOWN;

	spin_lock_irqsave(&vudc->lock, flags);

	list_for_each_entry(vreq, &vep->req_queue, list) {
		priv = virtio_usb_data_priv(vreq);
		if (req == &priv->req) {
			pr_debug("dequeue for vreq = %p, tag %llx\n", vreq,
				 (u64)(uintptr_t)vreq);
			// request will be completed from the completion handler
			priv->req.status = -ECONNRESET;
			rc = 0;
			break;
		}
	}
	endpoint = vep->ep_id | (usb_ep_dir_in(ep) ? VIRTIO_USB_EP_DIR_IN :
						     VIRTIO_USB_EP_DIR_OUT);
	spin_unlock_irqrestore(&vudc->lock, flags);

	if (rc)
		return rc;

	vreq = priv->vreq;

	cmd = virtio_usb_dc_cmd_alloc(vudc, VIRTIO_USB_CMD_DEV_CANCEL,
				      GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	cancel = virtio_usb_cmd_request(cmd);
	cancel->hdr.endpoint = cpu_to_le16(endpoint);
	cancel->hdr.port = cpu_to_le16(vudc->port);
	cancel->tag = cpu_to_le64((uintptr_t)vreq);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);

	return rc;
}

/**
 * virtio_ep_set_halt() - Sets/clears stall on selected endpoint
 * @ep: Endpoint object to set/clear stall on
 * @value: 1 for set stall, 0 for clear stall
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_ep_set_halt(struct usb_ep *ep, int value)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_dev_cmd_set_value *req;
	struct virtio_usb_dc *vudc = vep->vudc;
	struct virtio_usb_cmd *cmd;
	unsigned long flags;
	u16 endpoint;
	int rc;

	spin_lock_irqsave(&vudc->lock, flags);

	/*
	 * EP0 MUST NOT STALL if a control request is still pending.
	 * Composite expects -EAGAIN instead of a forced STALL, otherwise
	 * status stage collapses and the host sees EPROTO/EPIPE.
	 */
	if (&vep->ep == vudc->gadget.ep0) {
		if (value && !list_empty(&vep->req_queue)) {
			spin_unlock_irqrestore(&vudc->lock, flags);
			return -EAGAIN;
		}
	}

	if (value && ep->desc && usb_ep_dir_in(ep) &&
	    !list_empty(&vep->req_queue)) {
		spin_unlock_irqrestore(&vudc->lock, flags);
		return -EAGAIN;
	}
	endpoint = vep->ep_id | (usb_ep_dir_in(ep) ? VIRTIO_USB_EP_DIR_IN :
						     VIRTIO_USB_EP_DIR_OUT);
	spin_unlock_irqrestore(&vudc->lock, flags);

	cmd = virtio_usb_dc_cmd_alloc(vudc, VIRTIO_USB_CMD_DEV_EP_SET_HALT,
				      GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	req = virtio_usb_cmd_request(cmd);
	req->value = cpu_to_le32(value);
	req->hdr.endpoint = cpu_to_le16(endpoint);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);

	return rc;
}

/**
 * virtio_ep_set_wedge() - Set wedge on selected endpoint
 * @ep: Endpoint object
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_ep_set_wedge(struct usb_ep *ep)
{
	struct virtio_usb_ep *vep = usb_ep_to_virtio_ep(ep);
	struct virtio_usb_dc *vudc = vep->vudc;
	struct virtio_usb_dev_cmd_hdr *hdr;
	struct virtio_usb_cmd *cmd;
	unsigned long flags;
	u16 endpoint;
	int rc;

	spin_lock_irqsave(&vudc->lock, flags);
	endpoint = vep->ep_id | (usb_ep_dir_in(ep) ? VIRTIO_USB_EP_DIR_IN :
						     VIRTIO_USB_EP_DIR_OUT);
	spin_unlock_irqrestore(&vudc->lock, flags);

	cmd = virtio_usb_dc_cmd_alloc(vudc, VIRTIO_USB_CMD_DEV_EP_SET_WEDGE,
				      GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	hdr = virtio_usb_cmd_request(cmd);
	hdr->endpoint = cpu_to_le16(endpoint);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);

	return rc;
}

static const struct usb_ep_ops virtio_usb_ep_ops = {
	.enable = virtio_ep_enable,
	.disable = virtio_ep_disable,

	.alloc_request = virtio_ep_alloc_request,
	.free_request = virtio_ep_free_request,

	.queue = virtio_ep_queue,
	.dequeue = virtio_ep_dequeue,

	.set_halt = virtio_ep_set_halt,
	.set_wedge = virtio_ep_set_wedge,
};

/*-------------------------------------------------------------------------*/
/* UDC callbacks */

/**
 * virtio_usb_dc_set_selfpowered() - Sets the device selfpowered feature.
 * @gadget: The device being declared as self-powered
 * @is_selfpowered: Flag indicates if gadget is selfpowered
 *
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_dc_set_selfpowered(struct usb_gadget *gadget,
					 int is_selfpowered)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);
	unsigned int code = VIRTIO_USB_CMD_DEV_SET_SELF_POWERED;
	struct virtio_usb_dev_cmd_set_value *req;
	struct virtio_usb_cmd *cmd;

	cmd = virtio_usb_dc_cmd_alloc(vudc, code, GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	req = virtio_usb_cmd_request(cmd);
	req->value = cpu_to_le32(!!is_selfpowered);

	return virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);
}

/**
 * virtio_usb_dc_pullup() - Software-controlled connect/disconnect to USB host
 * @gadget: Pointer to the usb gadget structure.
 * @is_on: flag to start or stop
 *
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_dc_pullup(struct usb_gadget *gadget, int is_on)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);
	unsigned int code = VIRTIO_USB_CMD_DEV_PULLUP;
	struct virtio_usb_dev_cmd_set_value *req;
	struct virtio_usb_cmd *cmd;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&vudc->lock, flags);
	is_on = !!is_on;
	if (is_on == vudc->pullup) {
		rc = -EALREADY;
		goto on_unlock;
	}

	vudc->pullup = is_on;
	spin_unlock_irqrestore(&vudc->lock, flags);

	cmd = virtio_usb_dc_cmd_alloc(vudc, code, GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	req = virtio_usb_cmd_request(cmd);
	req->value = cpu_to_le32(is_on);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);

	return rc;

on_unlock:
	spin_unlock_irqrestore(&vudc->lock, flags);

	return rc;
}

/**
 * virtio_usb_dc_set_speed() - Sets maximum speed supported by gadget
 * @gadget: Pointer to the usb gadget structure.
 * @speed: The maximum speed to allowed to run
 */
static void virtio_usb_dc_set_speed(struct usb_gadget *gadget,
				    enum usb_device_speed speed)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);

	vudc->gadget.speed = min_t(u8, USB_SPEED_HIGH, speed);

	switch (speed) {
	case USB_SPEED_HIGH:
	case USB_SPEED_FULL:
		vudc->veps[0].ep.maxpacket = 64;
		break;
	case USB_SPEED_LOW:
		vudc->veps[0].ep.maxpacket = 8;
		break;
	default:
		break;
	}
}

/**
 * virtio_usb_dc_start() - Starts the device controller.
 * @gadget: Pointer to the usb gadget structure
 * @driver: Pointer to gadget driver structure
 *
 * Return: zero always
 */
static int virtio_usb_dc_start(struct usb_gadget *gadget,
			       struct usb_gadget_driver *driver)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);
	unsigned long flags;

	spin_lock_irqsave(&vudc->lock, flags);
	vudc->driver = driver;
	vudc->pullup = 0;
	spin_unlock_irqrestore(&vudc->lock, flags);

	return 0;
}

/**
 * virtio_usb_dc_stop() - Stops the device controller.
 * @gadget: Pointer to the usb gadget structure
 *
 * Return: zero always
 */
static int virtio_usb_dc_stop(struct usb_gadget *gadget)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);
	unsigned long flags;

	spin_lock_irqsave(&vudc->lock, flags);
	vudc->driver = NULL;
	vudc->pullup = 0;
	spin_unlock_irqrestore(&vudc->lock, flags);

	return 0;
}

/**
 * virtio_usb_dc_vbus_draw() - Constrain controller's VBUS power usage
 * @gadget: The device whose VBUS usage is being described
 * @mA: How much current to draw, in milliAmperes.
 *
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_dc_vbus_draw(struct usb_gadget *gadget, unsigned int mA)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);
	unsigned int code = VIRTIO_USB_CMD_DEV_VBUS_DRAW;
	struct virtio_usb_dev_cmd_set_value *req;
	struct virtio_usb_cmd *cmd;
	int rc;

	cmd = virtio_usb_dc_cmd_alloc(vudc, code, GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	req = virtio_usb_cmd_request(cmd);
	req->value = cpu_to_le32(mA);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);

	return rc;
}

/**
 * virtio_usb_dc_get_frame() - returns the current frame number
 * @gadget: controller that reports the frame number
 *
 * Return: Returns the usb frame number, normally eleven bits from
 * a SOF packet, or -errno on failure.
 */
static int virtio_usb_dc_get_frame(struct usb_gadget *gadget)
{
	struct virtio_usb_dc *vudc =
		container_of(gadget, struct virtio_usb_dc, gadget);
	unsigned int code = VIRTIO_USB_CMD_DEV_GET_FRAME_NUMBER;
	struct virtio_usb_dev_frame_number *resp;
	struct virtio_usb_cmd *cmd;
	int rc;

	cmd = virtio_usb_dc_cmd_alloc(vudc, code, GFP_ATOMIC);
	if (!cmd)
		return -ENOMEM;

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);
	if (rc)
		return rc;

	resp = virtio_usb_cmd_response(cmd);
	rc = le32_to_cpu(resp->frame_number);

	return rc;
}

static const struct usb_gadget_ops virtio_gadget_ops = {
	.set_selfpowered = virtio_usb_dc_set_selfpowered,
	.pullup = virtio_usb_dc_pullup,
	.udc_start = virtio_usb_dc_start,
	.udc_stop = virtio_usb_dc_stop,
	.vbus_draw = virtio_usb_dc_vbus_draw,
	.udc_set_speed = virtio_usb_dc_set_speed,
	.get_frame = virtio_usb_dc_get_frame,
};

/*-------------------------------------------------------------------------*/

/**
 * virtio_usb_dc_get_endpoint_count() - Function to get the number of
 * endpoints from the virtio-usb device
 * @vusb: Virtio usb device
 *
 * Context: Process context.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_dc_get_endpoint_count(struct virtio_usb_dc *vudc)
{
	unsigned int code = VIRTIO_USB_CMD_DEV_GET_ENDPOINT_COUNT;
	struct virtio_usb_dev_ep_count *ep_count;
	struct virtio_usb_cmd *cmd;
	int rc;

	cmd = virtio_usb_dc_cmd_alloc(vudc, code, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	virtio_usb_cmd_ref(cmd);
	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, NULL, cmd);
	if (rc)
		goto on_exit;

	ep_count = virtio_usb_cmd_response(cmd);
	vudc->neps = le32_to_cpu(ep_count->count);
	if (!vudc->neps)
		rc = -EINVAL;

on_exit:
	virtio_usb_cmd_unref(cmd);
	return rc;
}

/**
 * virtio_usb_dc_get_endpoint_info() - Function to get the endpoint
 * info from virtio-usb device.
 * @vusb: Virtio usb device
 *
 * Context: Process context.
 *  Return: 0 on success, -errno on failure.
 */
static int virtio_usb_dc_get_endpoint_info(struct virtio_usb_dc *vudc)
{
	unsigned int code = VIRTIO_USB_CMD_DEV_GET_ENDPOINT_INFO;
	struct virtio_usb_dev_ep_info *epinfo;
	struct virtio_usb_cmd *cmd;
	struct scatterlist sg;
	struct scatterlist *psg_data = &sg;
	int rc, i;

	vudc->veps = kcalloc(vudc->neps, sizeof(*vudc->veps) + sizeof(*epinfo),
			     GFP_KERNEL);
	if (!vudc->veps)
		return -ENOMEM;

	epinfo = (void *)vudc->veps + (sizeof(*vudc->veps) * vudc->neps);

	cmd = virtio_usb_dc_cmd_alloc(vudc, code, GFP_KERNEL);
	if (!cmd) {
		rc = -ENOMEM;
		goto on_error;
	}
	virtio_usb_cmd_ref(cmd);

	sg_init_one(psg_data, epinfo, sizeof(*epinfo) * vudc->neps);

	rc = virtio_usb_cmd_send_sync(vudc->vusb, NULL, psg_data, cmd);
	if (rc) {
		virtio_usb_cmd_unref(cmd);
		goto on_error;
	}

	INIT_LIST_HEAD(&vudc->gadget.ep_list);

	for (i = 0; i < vudc->neps; i++) {
		struct virtio_usb_ep *vep = &vudc->veps[i];
		u16 types = le16_to_cpu(epinfo[i].types);
		u16 directions = le16_to_cpu(epinfo[i].directions);
		struct usb_ep_caps caps = VIRTIO_USB_EP_CAPS(types, directions);
		unsigned int maxpacket_limit =
			le16_to_cpu(epinfo[i].maxpacket_limit);

		strscpy(vep->name, epinfo[i].name, sizeof(vep->name));
		vep->ep_id = i;
		vep->ep.caps = caps;
		vep->ep.name = vep->name;
		vep->ep.ops = &virtio_usb_ep_ops;
		INIT_LIST_HEAD(&vep->req_queue);
		list_add_tail(&vep->ep.ep_list, &vudc->gadget.ep_list);
		usb_ep_set_maxpacket_limit(&vep->ep, maxpacket_limit);
		vep->ep.max_streams = le16_to_cpu(epinfo[i].max_streams);
		vep->vudc = vudc;
	}
	virtio_usb_cmd_unref(cmd);
	vudc->gadget.ep0 = &vudc->veps[0].ep;
	list_del_init(&vudc->veps[0].ep.ep_list);

	return rc;

on_error:
	kfree(vudc->veps);
	vudc->veps = NULL;
	return rc;
}

static int virtio_usb_dc_parent_create(struct virtio_usb_dc *vudc)
{
	int rc;

	vudc->pdev = platform_device_alloc(GADGET_NAME, vudc->port);
	if (!vudc->pdev)
		return -ENOMEM;

	vudc->pdev->dev.parent = &vudc->vusb->vdev->dev;
	rc = platform_device_add(vudc->pdev);
	if (rc) {
		platform_device_put(vudc->pdev);
		vudc->pdev = NULL;
		return rc;
	}

	return 0;
}

/**
 * virtio_usb_dc_event_process() - Event process function
 * @event: virtio_usb_event
 *
 * Context: Process context.
 * Return: 0 on success, -errno on failure.
 */
static void virtio_usb_dc_event_process(struct virtio_usb_event *event)
{
	struct virtio_usb_dev_event *evt = virtio_usb_event_buf(event);
	struct virtio_usb_dev_setup_event *setup_evt;
	struct virtio_usb *vusb = event->vusb;
	struct virtio_usb_dc *vudc;
	struct usb_gadget_driver *driver;
	struct virtio_usb_ep *vep;
	unsigned long flags;
	int rc = 0;
	unsigned short port_id = le16_to_cpu(evt->port);

	if (!vusb->nports || port_id >= vusb->nports) {
		dev_err(&vusb->vdev->dev, "%s port[%d] index out of range\n",
			__func__, port_id);
		return;
	}

	vudc = vusb->vports[port_id].vudc;
	driver = vudc->driver;

	switch (le32_to_cpu(evt->code)) {
	case VIRTIO_USB_EVT_DEV_BIND: {
		if (vudc->registered) {
			dev_err(&vusb->vdev->dev,
				"port %d: BIND while still registered, ignoring\n",
				port_id);
			rc = -EBUSY;
			break;
		}

		memzero_explicit(&vudc->gadget, sizeof(struct usb_gadget));
		vudc->gadget.sg_supported = 1;
		rc = virtio_usb_dc_get_endpoint_count(vudc);
		if (rc) {
			dev_err(&vusb->vdev->dev,
				"Failed to get endpoint count\n");
			break;
		}
		rc = virtio_usb_dc_get_endpoint_info(vudc);
		if (rc) {
			dev_err(&vusb->vdev->dev,
				"Failed to get endpoint info\n");
			break;
		}
		if (!vudc->pdev) {
			rc = virtio_usb_dc_parent_create(vudc);
			if (rc) {
				dev_err(&vusb->vdev->dev,
					"Failed to create UDC parent device\n");
				kfree(vudc->veps);
				vudc->veps = NULL;
				break;
			}
		}
		vudc->gadget.name =
			kasprintf(GFP_KERNEL, "%s_%d", GADGET_NAME, port_id);
		if (!vudc->gadget.name) {
			rc = -ENOMEM;
			kfree(vudc->veps);
			vudc->veps = NULL;
			break;
		}
		vudc->gadget.ops = &virtio_gadget_ops;
		vudc->gadget.max_speed = USB_SPEED_HIGH;
		vudc->gadget.nonatomic = 1;

		//vudc->gadget.dev.init_name = gadget_name;
		vudc->gadget.dev.parent = &vudc->pdev->dev;
		rc = usb_add_gadget_udc(&vudc->pdev->dev, &vudc->gadget);
		if (rc) {
			dev_err(&vudc->pdev->dev, "Failed to add udc\n");
			kfree(vudc->veps);
			vudc->veps = NULL;
			kfree(vudc->gadget.name);
			break;
		}
		spin_lock_irqsave(&vudc->lock, flags);
		vudc->registered = 1;
		spin_unlock_irqrestore(&vudc->lock, flags);
		break;
	}
	case VIRTIO_USB_EVT_DEV_DISCONNECTED: {
		if (vudc->driver && vudc->driver->disconnect)
			vudc->driver->disconnect(&vudc->gadget);
		if (vudc->registered)
			usb_gadget_set_state(&vudc->gadget,
					     USB_STATE_NOTATTACHED);
		break;
	}
	case VIRTIO_USB_EVT_DEV_SETUP: {
		if (!driver)
			break;

		vep = usb_ep_to_virtio_ep(vudc->gadget.ep0);
		setup_evt = virtio_usb_event_buf(event);

		memcpy(&vep->setup, setup_evt->setup,
		       sizeof(struct usb_ctrlrequest));

		rc = driver->setup(&vudc->gadget,
				   (struct usb_ctrlrequest *)setup_evt->setup);
		if (rc < 0 && rc != -ESHUTDOWN)
			virtio_ep_set_halt(vudc->gadget.ep0, 1);
		break;
	}
	case VIRTIO_USB_EVT_DEV_RESET: {
		if (driver)
			usb_gadget_udc_reset(&vudc->gadget, driver);
		break;
	}
	case VIRTIO_USB_EVT_DEV_SUSPEND: {
		if (driver && driver->suspend)
			driver->suspend(&vudc->gadget);
		break;
	}
	case VIRTIO_USB_EVT_DEV_RESUME: {
		if (driver && driver->resume)
			driver->resume(&vudc->gadget);
		break;
	}
	case VIRTIO_USB_EVT_DEV_UNBIND: {
		unsigned int registered;

		spin_lock_irqsave(&vudc->lock, flags);
		registered = vudc->registered;
		vudc->registered = 0;
		spin_unlock_irqrestore(&vudc->lock, flags);

		if (registered) {
			usb_del_gadget_udc(&vudc->gadget);
			kfree(vudc->gadget.name);
			vudc->gadget.name = NULL;
		}

		kfree(vudc->veps);
		vudc->veps = NULL;

		if (vudc->pdev) {
			platform_device_unregister(vudc->pdev);
			vudc->pdev = NULL;
		}
		break;
	}
	default:
		rc = -EINVAL;
		break;
	}
}

/**
 * virtio_usb_dc_event_work() - Worker to get all events
 * from the virtqueue and process them.
 * @work: Kernel work to handle event completion.
 *
 * Context: Process context.
 */
void virtio_usb_dc_event_work(struct work_struct *work)
{
	struct virtio_usb *vusb =
		container_of(work, struct virtio_usb, vq_dev_event_work);
	struct virtio_usb_queue *evtq =
		&vusb->vqueues[vusb->dev_vq_base + VIRTIO_USB_VQ_EVENT_IDX];

	virtio_usb_evt_work(evtq, virtio_usb_dc_event_process);
}

/**
 * virtio_usb_dc_event_populate() - Add events to the device event queue.
 * @vusb: VirtIO USB device
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_dc_event_populate(struct virtio_usb *vusb)
{
	struct virtio_usb_queue *evt_queue =
		&vusb->vqueues[vusb->dev_vq_base + VIRTIO_USB_VQ_EVENT_IDX];
	struct virtio_usb_event *events;
	int rc;

	events = virtio_usb_events_alloc(vusb, evt_queue,
					 sizeof(struct virtio_usb_dev_event));

	if (!events)
		return -ENOMEM;

	rc = virtio_usb_events_populate(events);

	return rc;
}

/**
 * virtio_usb_dc_init() - Initializes the device role.
 * @vusb: VirtIO USB device
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_dc_init(struct virtio_usb *vusb, int port_id)
{
	struct virtio_usb_dc *vudc;
	unsigned int i;

	vudc = devm_kzalloc(&vusb->vdev->dev, sizeof(*vudc), GFP_KERNEL);
	if (!vudc)
		return -ENOMEM;

	vusb->vports[port_id].vudc = vudc;
	vudc->vusb = vusb;
	vudc->port = port_id;
	for (i = 0; i < VIRTIO_USB_VQ_DEV_MAX; i++)
		vudc->dcqs[i] = &vusb->vqueues[vusb->dev_vq_base + i];

	spin_lock_init(&vudc->lock);

	vudc->registered = 0;

	return 0;
}

/**
 * virtio_usb_dc_deinit() - Deinitialize the device role.
 * @vusb: VirtIO USB device
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_dc_deinit(struct virtio_usb *vusb)
{
	int port_id;

	if (!vusb->device_role)
		return -ENODEV;

	for (port_id = 0; port_id < vusb->nports; port_id++) {
		struct virtio_usb_dc *vudc = vusb->vports[port_id].vudc;
		unsigned int registered;
		unsigned long flags;

		if (!vudc)
			continue;

		spin_lock_irqsave(&vudc->lock, flags);
		registered = vudc->registered;
		spin_unlock_irqrestore(&vudc->lock, flags);

		if (registered)
			usb_del_gadget_udc(&vudc->gadget);

		kfree(vudc->veps);
		vudc->veps = NULL;

		if (vudc->pdev) {
			platform_device_unregister(vudc->pdev);
			vudc->pdev = NULL;
		}

		vusb->vports[port_id].vudc = NULL;
	}

	return 0;
}

/**
 * virtio_usb_dc_dataq_stop_cb() - Stops the data virtqueue
 * @vusb: VirtIO usb device.
 * @dataq: data virtqueue wrapper
 *
 * Context: Any context.
 */
static void virtio_usb_dc_dataq_stop_cb(struct virtio_usb *vusb,
					struct virtio_usb_queue *dataq)
{
	int port_id;

	if (!vusb->device_role)
		return;

	virtio_usb_dataq_stop_cb(vusb, dataq);
	cancel_work_sync(&vusb->vq_dev_data_rx_work);

	for (port_id = 0; port_id < vusb->nports; port_id++) {
		struct virtio_usb_dc *vudc = vusb->vports[port_id].vudc;
		struct virtio_usb_data *vreq, *vreq_tmp;
		struct virtio_usb_dc_priv *priv;
		unsigned int i;

		if (!vudc)
			continue;

		for (i = 0; i < vudc->neps; i++) {
			list_for_each_entry_safe(vreq, vreq_tmp,
						 &vudc->veps[i].req_queue,
						 list) {
				priv = virtio_usb_data_priv(vreq);
				priv->req.status = -ESHUTDOWN;
				list_del_init(&vreq->list);
				virtio_usb_data_unref(vreq);
				usb_gadget_giveback_request(&priv->vep->ep,
							    &priv->req);
			}
		}
	}
}

/**
 * virtio_usb_dc_data_notify_cb() - Data virtqueue notification callback
 * @vqueue: Underlying event virtqueue.
 *
 * This callback function is called upon a vring interrupt request from the
 * device.
 *
 * Context: Interrupt context.
 */
static void virtio_usb_dc_data_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;

	schedule_work(&vusb->vq_dev_data_rx_work);
}

/**
 * virtio_usb_dc_evt_notify_cb() - Event virtqueue notification callback
 * @vqueue: Underlying event virtqueue.
 *
 * This callback function is called upon a vring interrupt request from the
 * device.
 *
 * Context: Interrupt context.
 */
static void virtio_usb_dc_evt_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;

	schedule_work(&vusb->vq_dev_event_work);
}

/**
 * virtio_usb_dc_evtq_stop_cb() - Stops the event virtqueue.
 * @vusb: VirtIO usb device.
 * @vq: virtio usb vq wrapper
 *
 * Context: Any context.
 */
static void virtio_usb_dc_evtq_stop_cb(struct virtio_usb *vusb,
				       struct virtio_usb_queue *vq)
{
	/*
	 * Unlike the host evtq stop path (which safely updates in-memory
	 * port status bits), dc event processing drives the UDC state
	 * machine and calls back into gadget drivers and UDC core. Doing
	 * so here, when the UDC may be only partially initialized (probe
	 * failure) or already torn down (remove path), risks
	 * use-after-free and crashes - virtio_usb_evt_drain_stop_cb()
	 * drains without processing for exactly this reason.
	 */
	cancel_work_sync((struct work_struct *)&vusb->vq_dev_event_work);
	virtio_usb_evt_drain_stop_cb(vq, NULL);
}

const struct virtio_usb_vq_desc dev_vqueues[VIRTIO_USB_VQ_DEV_MAX] = {
			[VIRTIO_USB_VQ_COMMAND_IDX] = {
			.callback = virtio_usb_cmd_notify_cb,
			.name = "virtusb-dev-cmd",
			.process = virtio_usb_cmd_process_cb,
			.stop = virtio_usb_cmdq_stop_cb,
		},
		[VIRTIO_USB_VQ_EVENT_IDX] = {
			.callback = virtio_usb_dc_evt_notify_cb,
			.name = "virtusb-dev-evt",
			.process = NULL,
			.stop = virtio_usb_dc_evtq_stop_cb,
		},
		[VIRTIO_USB_VQ_DATA_IDX] = {
			.callback = virtio_usb_dc_data_notify_cb,
			.name = "virtusb-dev-data",
			.process = NULL,
			.stop = virtio_usb_dc_dataq_stop_cb,
		},
};
