// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * virtio-usb: Virtio usb device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include "controller.h"
#include "vq_common.h"

/**
 * struct virtio_usb_cmd_generic_hdr - Generic command header
 * for the command
 * @code: command code
 * @value: value for the command
 */
struct virtio_usb_cmd_generic_hdr {
	__le32 code;
	__le32 value;
};

/**
 * virtio_error_to_usb - Convert virtio error to usb error
 * @error: virtio error code
 *
 * Context: Any context.
 * Return: virtio error converted to usb error code
 */
int virtio_error_to_usb(unsigned int error)
{
	int status;

	switch (error) {
	case VIRTIO_USB_S_OK:
		status = 0;
		break;
	case VIRTIO_USB_S_ERR_CANCELLED:
		/* cancelled */
		status = -ECONNRESET;
		break;
		/* device shutdown or removal*/
	case VIRTIO_USB_S_ERR_NO_DEVICE:
		status = -ESHUTDOWN;
		break;
	case VIRTIO_USB_S_ERR_STALL:
		status = -EPIPE;
		break;
	case VIRTIO_USB_S_ERR_OVERFLOW:
		status = -EOVERFLOW;
		break;
	case VIRTIO_USB_S_ERR_SHORT_PKT:
		/* short packet */
		status = -EREMOTEIO;
		break;
	case VIRTIO_USB_S_ERR_BAD_MSG:
		status = -EINVAL;
		break;
	case VIRTIO_USB_S_ERR_DATA_IN:
		status = -ECOMM;
		break;
	case VIRTIO_USB_S_ERR_DATA_OUT:
		status = -ENOSR;
		break;
	case VIRTIO_USB_S_ERR_ISO_XFER:
		status = -EXDEV;
		break;
	case VIRTIO_USB_S_ERR_ISO_BIG:
		status = -EFBIG;
		break;
	case VIRTIO_USB_S_ERR_MSG_SIZE:
		status = -EMSGSIZE;
		break;
	case VIRTIO_USB_S_ERR_INTERNAL:
	default:
		status = -EPROTO;
		break;
	}
	return status;
}

/**
 * virtio_usb_msg_ref() - Increment reference counter for the message.
 * @msg: common message.
 *
 * Context: Any context.
 */
static void virtio_usb_msg_ref(struct virtio_usb_msg_common *msg)
{
	refcount_inc(&msg->ref_count);
}

/**
 * virtio_usb_msg_unref() - Decrement reference counter for the message.
 * @msg: common message.
 *
 * The message will be freed when the ref_count value is 0.
 *
 * Context: Any context.
 */
static void virtio_usb_msg_unref(struct virtio_usb_msg_common *msg)
{
	if (refcount_dec_and_test(&msg->ref_count))
		kfree(msg);
}

/**
 * virtio_usb_msg_request() - Get a pointer to the request header.
 * @msg: common message.
 *
 * Context: Any context.
 */
static void *virtio_usb_msg_request(struct virtio_usb_msg_common *msg)
{
	return sg_virt(&msg->sg_request);
}

/**
 * virtio_usb_msg_response() - Get a pointer to the response header.
 * @msg: common message.
 *
 * Context: Any context.
 */
static void *virtio_usb_msg_response(struct virtio_usb_msg_common *msg)
{
	return sg_virt(&msg->sg_response);
}

/**
 * virtio_usb_msg_alloc() - Allocate and initialize a message.
 * @msg_size: Size of the requested message.
 * @request_size: Size of request header.
 * @response_size: Size of response header.
 * @gfp: Kernel flags for memory allocation.
 *
 * The message will be automatically freed when the ref_count value is 0.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated message on success, NULL on failure.
 */
static void *virtio_usb_msg_alloc(size_t msg_size, size_t request_size,
				  size_t response_size, gfp_t gfp)
{
	struct virtio_usb_msg_common *msg;

	if (!msg_size || !request_size || !response_size)
		return NULL;

	msg = kzalloc(msg_size + request_size + response_size, gfp);
	if (!msg)
		return NULL;

	sg_init_one(&msg->sg_request, (u8 *)msg + msg_size, request_size);
	sg_init_one(&msg->sg_response, (u8 *)msg + msg_size + request_size,
		    response_size);

	refcount_set(&msg->ref_count, 1);

	return msg;
}

/**
 * virtio_usb_cmd_ref() - Increment reference counter for the command.
 * @cmd: Command message.
 *
 * Context: Any context.
 */
void virtio_usb_cmd_ref(struct virtio_usb_cmd *cmd)
{
	virtio_usb_msg_ref((struct virtio_usb_msg_common *)cmd);
}

/**
 * virtio_usb_cmd_unref() - Decrement reference counter for the command.
 * @cmd: Command message.
 *
 * The message will be freed when the ref_count value is 0.
 *
 * Context: Any context.
 */
void virtio_usb_cmd_unref(struct virtio_usb_cmd *cmd)
{
	virtio_usb_msg_unref((struct virtio_usb_msg_common *)cmd);
}

/**
 * virtio_usb_cmd_request() - Get a pointer to the request header.
 * @cmd: Command msg.
 *
 * Context: Any context.
 */
void *virtio_usb_cmd_request(struct virtio_usb_cmd *cmd)
{
	return virtio_usb_msg_request((struct virtio_usb_msg_common *)cmd);
}

/**
 * virtio_usb_cmd_response() - Get a pointer to the response header.
 * @cmd: command message.
 *
 * Context: Any context.
 */
void *virtio_usb_cmd_response(struct virtio_usb_cmd *cmd)
{
	return virtio_usb_msg_response((struct virtio_usb_msg_common *)cmd);
}

/**
 * virtio_usb_cmd_alloc() - Allocate and initialize a control message.
 * @request_size: Size of request header.
 * @response_size: Size of response header.
 * @gfp: Kernel flags for memory allocation.
 *
 * The message will be automatically freed when the ref_count value is 0.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated message on success, NULL on failure.
 */
struct virtio_usb_cmd *virtio_usb_cmd_alloc(size_t request_size,
					    size_t response_size, gfp_t gfp)
{
	struct virtio_usb_cmd *cmd;

	if (!request_size || !response_size)
		return NULL;

	cmd = virtio_usb_msg_alloc(sizeof(*cmd), request_size, response_size,
				   gfp);
	if (!cmd)
		return NULL;

	init_completion(&cmd->notify);

	return cmd;
}

/**
 * virtio_usb_cmd_msg_send() - Function to send command message to the
 * command virtqueue
 * @vusb: VirtIO usb device.
 * @msg: common message.
 * @out_sgs: Additional sg-list to attach to the request header (may be NULL).
 * @in_sgs: Additional sg-list to attach to the response header (may be NULL).
 *
 * Context: Any context. Takes and releases the command queue spinlock.
 * Return: 0 on success, -errno on failure.
 */
static int virtio_usb_cmd_msg_send(struct virtio_usb *vusb,
				   struct virtio_usb_msg_common *msg,
				   struct scatterlist *out_sgs,
				   struct scatterlist *in_sgs)
{
	struct virtio_usb_queue *queue = msg->queue;
	unsigned int nouts = 0, nins = 0;
	struct scatterlist *psgs[4];
	bool notify = false;
	unsigned long flags;
	int rc = 0;

	psgs[nouts++] = &msg->sg_request;
	if (out_sgs)
		psgs[nouts++] = out_sgs;

	psgs[nouts + nins++] = &msg->sg_response;
	if (in_sgs)
		psgs[nouts + nins++] = in_sgs;

	spin_lock_irqsave(&queue->lock, flags);
	rc = virtqueue_add_sgs(queue->vqueue, psgs, nouts, nins, msg,
			       GFP_ATOMIC);
	if (!rc)
		notify = virtqueue_kick_prepare(queue->vqueue);
	spin_unlock_irqrestore(&queue->lock, flags);

	if (rc)
		goto on_exit;

	if (notify)
		virtqueue_notify(queue->vqueue);

on_exit:
	return rc;
}

/**
 * virtio_usb_cmd_send() - Send a command to the command virtqueue
 * @vusb: VirtIO usb device.
 * @cmd: command message.
 * @out_sgs: Additional sg-list to attach to the request header (may be NULL).
 * @in_sgs: Additional sg-list to attach to the response header (may be NULL).
 * @nowait: Flag indicating whether to wait for completion.
 *
 * Context: Any context. Takes and releases the command queue spinlock.
 *          May sleep if @nowait is false.
 * Return: The return value is a message status code (VIRTIO_USB_S_XXX) converted to an
 * appropriate -errno value.
 */
int virtio_usb_cmd_send(struct virtio_usb *vusb, struct virtio_usb_cmd *cmd,
			struct scatterlist *out_sgs, struct scatterlist *in_sgs,
			bool nowait)
{
	unsigned int js = msecs_to_jiffies(virtio_usb_cmd_timeout_ms);
	struct virtio_usb_cmd_generic_hdr *request =
		virtio_usb_cmd_request(cmd);
	struct virtio_usb_cmd_status *response = virtio_usb_cmd_response(cmd);
	struct virtio_device *vdev = vusb->vdev;
	int rc;
	u32 code;

	/* Set the default status in case the command was canceled. */
	response->code = cpu_to_le32(VIRTIO_USB_S_ERR_CANCELLED);

	virtio_usb_cmd_ref(cmd);

	rc = virtio_usb_cmd_msg_send(vusb, (struct virtio_usb_msg_common *)cmd,
				     out_sgs, in_sgs);
	if (rc) {
		dev_err(&vdev->dev, "failed to send control message (0x%08x)\n",
			le32_to_cpu(request->code));

		/*
		 * Since in this case virtio_usb_cmd_process_cb() will not be
		 * called, it is necessary to decrement the reference count.
		 */
		virtio_usb_cmd_unref(cmd);
		goto on_exit;
	}

	if (nowait)
		goto on_exit;

	rc = wait_for_completion_interruptible_timeout(&cmd->notify, js);
	if (rc <= 0) {
		if (!rc) {
			dev_err(&vdev->dev,
				"control message (0x%08x) timeout\n",
				le32_to_cpu(request->code));
			rc = -ETIMEDOUT;
		}

		goto on_exit;
	}

	code = le32_to_cpu(response->code);

	rc = virtio_error_to_usb(code);

on_exit:
	virtio_usb_cmd_unref(cmd);
	return rc;
}

/**
 * virtio_usb_events_alloc() - Allocates the events.
 * @vusb: VirtIO USB device
 * @vq: event virtqueue to which events need to be populated.
 * @evt_size: Size of the event structure.
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
struct virtio_usb_event *virtio_usb_events_alloc(struct virtio_usb *vusb,
						 struct virtio_usb_queue *vq,
						 size_t evt_size)
{
	unsigned int n = virtqueue_get_vring_size(vq->vqueue);
	struct virtio_device *vdev = vusb->vdev;
	struct virtio_usb_event *events, *event;
	unsigned int i;

	events = devm_kcalloc(&vdev->dev, n, (sizeof(*events) + evt_size),
			      GFP_KERNEL);
	if (!events)
		return NULL;

	for (i = 0; i < n; i++) {
		event = (void *)events + i * (sizeof(*event) + evt_size);
		event->evt_size = evt_size;
		event->queue = vq;
		sg_init_one(&event->sg_event, (void *)event + sizeof(*event),
			    evt_size);
		event->vusb = vusb;
	}
	return events;
}

/**
 * virtio_usb_events_populate() - Add preallocated events to the event queue.
 * @events: Pointer to preallocated virtio usb events
 *
 * Context: Any context.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_events_populate(struct virtio_usb_event *events)
{
	unsigned int n = virtqueue_get_vring_size(events[0].queue->vqueue);
	size_t evt_size = events[0].evt_size;
	struct virtio_usb_event *event;
	unsigned int i, rc = 0;

	for (i = 0; i < n; i++) {
		event = (void *)events + i * (sizeof(*event) + evt_size);
		rc = virtqueue_add_inbuf(event->queue->vqueue, &event->sg_event,
					 1, event, GFP_KERNEL);
		if (rc)
			return rc;
	}
	/* Notify the backend that event buffers are available so it can
	 * deliver any pending PORT_CONNECTED events immediately.
	 */
	virtqueue_notify(events[0].queue->vqueue);
	return rc;
}

/**
 * virtio_usb_event_buf() - Get the event buffer.
 * @event: The virtio_usb_event
 *
 * Context: Any context.
 * Return: Pointer to the event buffer
 */
void *virtio_usb_event_buf(struct virtio_usb_event *event)
{
	return sg_virt(&event->sg_event);
}

/**
 * virtio_usb_event_send() - Send an event to the specified event queue.
 * @event: The event that needs to be send
 *
 *
 * Context: Any context which expects the event queue spinlock to be held by
 *          caller.
 * Return: 0 on success -errno on failure
 */
int virtio_usb_event_send(struct virtio_usb_event *event)
{
	int rc = 0;
	void *event_buf = virtio_usb_event_buf(event);

	/* reset event content */
	memset(event_buf, 0, event->evt_size);

	rc = virtqueue_add_inbuf(event->queue->vqueue, &event->sg_event, 1,
				 event, GFP_ATOMIC);
	if (rc)
		return rc;

	if (virtqueue_kick_prepare(event->queue->vqueue))
		virtqueue_notify(event->queue->vqueue);
	return rc;
}

/**
 * virtio_usb_data_ref() - Increment reference counter for the data.
 * @data: Data message.
 *
 * Context: Any context.
 */
void virtio_usb_data_ref(struct virtio_usb_data *data)
{
	virtio_usb_msg_ref((struct virtio_usb_msg_common *)data);
}

/**
 * virtio_usb_data_unref() - Decrement reference counter for the data.
 * @data: Data message.
 *
 * The data will be freed when the ref_count value is 0.
 *
 * Context: Any context.
 */
void virtio_usb_data_unref(struct virtio_usb_data *data)
{
	virtio_usb_msg_unref((struct virtio_usb_msg_common *)data);
}

/**
 * virtio_usb_data_request() - Get a pointer to the request header.
 * @data: Data message.
 *
 * Context: Any context.
 */
void *virtio_usb_data_request(struct virtio_usb_data *data)
{
	return virtio_usb_msg_request((struct virtio_usb_msg_common *)data);
}

/**
 * virtio_usb_data_priv() - Get a pointer to the data priv.
 * @data: Data message.
 *
 * Context: Any context.
 */
void *virtio_usb_data_priv(struct virtio_usb_data *data)
{
	if (!data)
		return NULL;

	return data->priv;
}

/**
 * virtio_usb_data_response() - Get a pointer to the response header.
 * @data: Data message.
 *
 * Context: Any context.
 */
void *virtio_usb_data_response(struct virtio_usb_data *data)
{
	return virtio_usb_msg_response((struct virtio_usb_msg_common *)data);
}

/**
 * virtio_usb_data_alloc() - Allocate and initialize a data message.
 * @request_size: Size of request header.
 * @response_size: Size of response header.
 * @priv_size: Size of priv context of the data.
 * @gfp: Kernel flags for memory allocation.
 *
 * The message will be automatically freed when the ref_count value is 0.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated message on success, NULL on failure.
 */
struct virtio_usb_data *virtio_usb_data_alloc(size_t request_size,
					      size_t response_size,
					      size_t priv_size, gfp_t gfp)
{
	struct virtio_usb_data *data;

	if (!request_size || !response_size || !priv_size)
		return NULL;

	data = virtio_usb_msg_alloc(sizeof(*data) + priv_size, request_size,
				    response_size, gfp);
	if (!data)
		return NULL;
	data->priv = (u8 *)data + sizeof(*data);

	return data;
}

/**
 * virtio_usb_data_send() - Send a data message
 * @vusb: VirtIO usb device.
 * @data: Data message.
 * @out_sgs: Additional sg-list to attach to the request header
 * @in_sgs: Additional sg-list to attach to the response header
 *
 * Context: Any context. Takes and releases the data queue spinlock.
 * Return: 0 on success, -errno on failure.
 */
int virtio_usb_data_send(struct virtio_usb *vusb, struct virtio_usb_data *data,
			 struct scatterlist *out_sgs,
			 struct scatterlist *in_sgs)
{
	struct virtio_usb_queue *queue = data->msg.queue;
	struct scatterlist *psgs[4] = { NULL };
	unsigned int nouts = 0, nins = 0;
	bool notify = false;
	int rc = 0;

	psgs[nouts++] = &data->msg.sg_request;
	if (out_sgs)
		psgs[nouts++] = out_sgs;

	psgs[nouts + nins++] = &data->msg.sg_response;
	if (in_sgs)
		psgs[nouts + nins++] = in_sgs;

	spin_lock_irq(&queue->lock);
	rc = virtqueue_add_sgs(queue->vqueue, psgs, nouts, nins, data,
			       GFP_ATOMIC);
	if (!rc)
		notify = virtqueue_kick_prepare(queue->vqueue);
	spin_unlock_irq(&queue->lock);

	if (rc)
		goto on_exit;

	if (notify)
		virtqueue_notify(queue->vqueue);

on_exit:
	return rc;
}

/**
 * virtio_usb_cmd_notify_cb() - command virtqueue
 * notification callback
 * @vqueue: Underlying virtqueue.
 *
 * This callback function is called upon a vring interrupt request from the
 * device.
 *
 * Context: Interrupt context.
 */
void virtio_usb_cmd_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_usb *vusb = vqueue->vdev->priv;
	struct virtio_usb_queue *vq = &vusb->vqueues[vqueue->index];
	unsigned long flags;
	u32 length;
	void *buf;

	spin_lock_irqsave(&vq->lock, flags);
	do {
		virtqueue_disable_cb(vqueue);
		while ((buf = virtqueue_get_buf(vqueue, &length)))
			vq->process(vusb, buf);
		if (unlikely(virtqueue_is_broken(vqueue)))
			break;
	} while (!virtqueue_enable_cb(vqueue));
	spin_unlock_irqrestore(&vq->lock, flags);
}

/**
 * virtio_usb_cmd_process_cb() - process callback for commands.
 * @vusb: VirtIO usb device.
 * @buf: Pointer to the command message
 *
 * Context: Interrupt context.  Expects the command queue spinlock to be held by
 *          caller.
 */
void virtio_usb_cmd_process_cb(struct virtio_usb *vusb, void *buf)
{
	struct virtio_usb_cmd *cmd = (struct virtio_usb_cmd *)buf;

	complete(&cmd->notify);
	virtio_usb_cmd_unref(cmd);
}

/**
 * virtio_usb_cmdq_stop_cb() - Stops the command virtqueue
 * @vusb: VirtIO usb device.
 * @cmdq: The command virtqueue to be stopped
 *
 * Context: Any context.
 */
void virtio_usb_cmdq_stop_cb(struct virtio_usb *vusb,
			     struct virtio_usb_queue *cmdq)
{
	struct virtio_usb_cmd *cmd;
	unsigned long flags;

	if (cmdq->vqueue) {
		spin_lock_irqsave(&cmdq->lock, flags);
		virtqueue_disable_cb(cmdq->vqueue);

		while ((cmd = virtqueue_detach_unused_buf(cmdq->vqueue)))
			cmdq->process(vusb, cmd);

		spin_unlock_irqrestore(&cmdq->lock, flags);
	}
}

/**
 * virtio_usb_dataq_stop_cb() - Stops the data virtqueue
 * @vusb: VirtIO usb device.
 * @dataq: The data virtqueue to be stopped.
 *
 * Context: Any context.
 */
void virtio_usb_dataq_stop_cb(struct virtio_usb *vusb,
			      struct virtio_usb_queue *dataq)
{
	if (dataq->vqueue) {
		spin_lock_irq(&dataq->lock);
		virtqueue_disable_cb(dataq->vqueue);
		spin_unlock_irq(&dataq->lock);
	}
}

/**
 * virtio_usb_evt_work() - Generic event queue receive worker.
 * @evtq: The event virtqueue to drain.
 * @process_one: Callback invoked for each dequeued event, with the
 *               queue's own lock released (the callback is free to
 *               sleep / send further virtio commands).
 *
 * Common drain loop shared by every role's own event queue: dequeue
 * completed event buffers, hand each one to @process_one, then
 * immediately re-arm and resend it via virtio_usb_event_send() so the
 * backend always has a full set of event buffers available.
 *
 * Context: Process context.
 */
void virtio_usb_evt_work(struct virtio_usb_queue *evtq,
			 void (*process_one)(struct virtio_usb_event *event))
{
	u32 length;
	struct virtio_usb_event *event;

	spin_lock(&evtq->lock);
	do {
		while ((event = virtqueue_get_buf(evtq->vqueue, &length))) {
			spin_unlock(&evtq->lock);
			process_one(event);
			spin_lock(&evtq->lock);
			virtio_usb_event_send(event);
		}
		if (unlikely(virtqueue_is_broken(evtq->vqueue)))
			break;
	} while (!virtqueue_enable_cb(evtq->vqueue));
	spin_unlock(&evtq->lock);
}

/**
 * virtio_usb_evt_drain_stop_cb() - Generic event queue stop callback.
 * @vq: The event virtqueue to stop.
 * @work: The work item that drains @vq via virtio_usb_evt_work(), to
 *        be cancelled before draining (may be NULL if the caller has
 *        already cancelled it, or must defer cancellation itself).
 *
 * Do not process events during teardown - whatever state process_one()
 * would touch may already be partially torn down (probe failure) or
 * gone (remove path). Just drain the used ring without processing, so
 * virtio core's del_vqs() finds it empty.
 *
 * Context: Any context that permits to sleep (if @work is non-NULL).
 */
void virtio_usb_evt_drain_stop_cb(struct virtio_usb_queue *vq,
				  struct work_struct *work)
{
	unsigned long flags;
	u32 length;
	void *buf;

	if (!vq->vqueue)
		return;

	if (work)
		cancel_work_sync(work);

	spin_lock_irqsave(&vq->lock, flags);
	virtqueue_disable_cb(vq->vqueue);
	while ((buf = virtqueue_get_buf(vq->vqueue, &length)))
		;
	spin_unlock_irqrestore(&vq->lock, flags);
}
