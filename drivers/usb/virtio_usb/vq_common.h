/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * virtio-usb: Virtio usb device
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef VIRTIO_USB_COMMON_H
#define VIRTIO_USB_COMMON_H

#include <linux/atomic.h>
#include <linux/virtio.h>

#include "controller.h"

/**
 * struct virtio_usb_msg_common - Common message structure
 * for command and data message
 * @sg_request: Scattergather list containing a device request (header).
 * @sg_response: Scattergather list containing a device response (status).
 * @queue: Virtqueue wrapper
 * @ref_count: Reference count used to manage a message lifetime.
 */
struct virtio_usb_msg_common {
	struct scatterlist sg_request;
	struct scatterlist sg_response;
	struct virtio_usb_queue *queue;
	refcount_t ref_count;
};

/**
 * struct virtio_usb_cmd - Command message
 * @msg: Common message
 * @notify: Request completed notification.
 */
struct virtio_usb_cmd {
	struct virtio_usb_msg_common msg;
	struct completion notify;
};

void virtio_usb_cmd_ref(struct virtio_usb_cmd *cmd);
void virtio_usb_cmd_unref(struct virtio_usb_cmd *cmd);
void *virtio_usb_cmd_request(struct virtio_usb_cmd *cmd);
void *virtio_usb_cmd_response(struct virtio_usb_cmd *cmd);

struct virtio_usb_cmd *virtio_usb_cmd_alloc(size_t request_size,
					    size_t response_size, gfp_t gfp);
int virtio_usb_cmd_send(struct virtio_usb *vusb, struct virtio_usb_cmd *cmd,
			struct scatterlist *out_sgs, struct scatterlist *in_sgs,
			bool nowait);

/**
 * virtio_usb_cmd_send_sync - Simplified sending of synchronous message.
 * @vusb: VirtIO usb device.
 * @out_sgs: Additional sg-list to attach to the request header (may be NULL).
 * @in_sgs: Additional sg-list to attach to the response header (may be NULL).
 * @cmd: Command message.
 *
 * After returning from this function, the message will be deleted. If message
 * content is still needed, the caller must additionally to
 * virtio_usb_cmd_ref/unref() it.
 *
 * The msg_timeout_ms module parameter defines the message completion timeout.
 * If the message is not completed within this time, the function will return an
 * error.
 *
 * Context: Any context that permits to sleep.
 * Return: 0 on success, -errno on failure.
 *
 * The return value is a message status code (VIRTIO_USB_S_XXX) converted to an
 * appropriate -errno value.
 */
static inline int virtio_usb_cmd_send_sync(struct virtio_usb *vusb,
					   struct scatterlist *out_sgs,
					   struct scatterlist *in_sgs,
					   struct virtio_usb_cmd *cmd)
{
	return virtio_usb_cmd_send(vusb, cmd, out_sgs, in_sgs, false);
}

/**
 * virtio_usb_cmd_send_async() - Simplified sending of asynchronous message.
 * @vusb: VirtIO usb device.
 * @out_sgs: Additional sg-list to attach to the request header (may be NULL).
 * @in_sgs: Additional sg-list to attach to the response header (may be NULL).
 * @cmd: Command message..
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static inline int virtio_usb_cmd_send_async(struct virtio_usb *vusb,
					    struct scatterlist *out_sgs,
					    struct scatterlist *in_sgs,
					    struct virtio_usb_cmd *cmd)
{
	return virtio_usb_cmd_send(vusb, cmd, out_sgs, in_sgs, true);
}

/**
 * struct virtio_usb_data - Data message.
 * @msg: Common message
 * @list: VirtIO usb data list entry.
 * @priv: Pointer to priv structure.
 */
struct virtio_usb_data {
	struct virtio_usb_msg_common msg;
	struct list_head list;
	void *priv;
};

void *virtio_usb_data_request(struct virtio_usb_data *data);
void *virtio_usb_data_response(struct virtio_usb_data *data);
void *virtio_usb_data_priv(struct virtio_usb_data *data);
void virtio_usb_data_ref(struct virtio_usb_data *data);
void virtio_usb_data_unref(struct virtio_usb_data *data);

struct virtio_usb_data *virtio_usb_data_alloc(size_t request_size,
					      size_t response_size,
					      size_t priv_size, gfp_t gfp);

int virtio_usb_data_send(struct virtio_usb *vusb, struct virtio_usb_data *data,
			 struct scatterlist *out_sgs,
			 struct scatterlist *in_sgs);

void virtio_usb_cmd_notify_cb(struct virtqueue *vqueue);

void virtio_usb_cmd_process_cb(struct virtio_usb *vusb, void *value);

void virtio_usb_cmdq_stop_cb(struct virtio_usb *vusb,
			     struct virtio_usb_queue *vq);
void virtio_usb_dataq_stop_cb(struct virtio_usb *vusb,
			      struct virtio_usb_queue *vq);

/**
 * struct virtio_usb_event - Event message.
 * @work: Optional Kernel work to handle the event.
 * @sg_event: Scattergather list containing a event.
 * @queue: Virtqueue wrqapper
 * @vusb: Virtio usb device
 * @evt_size: size of event buffer
 */
struct virtio_usb_event {
	struct work_struct work;
	struct scatterlist sg_event;
	struct virtio_usb_queue *queue;
	struct virtio_usb *vusb;
	size_t evt_size;
};

struct virtio_usb_event *virtio_usb_events_alloc(struct virtio_usb *vusb,
						 struct virtio_usb_queue *vq,
						 size_t evt_size);
int virtio_usb_events_populate(struct virtio_usb_event *events);
int virtio_usb_event_send(struct virtio_usb_event *event);
void *virtio_usb_event_buf(struct virtio_usb_event *event);
int virtio_error_to_usb(unsigned int error);

void virtio_usb_evt_work(struct virtio_usb_queue *evtq,
			 void (*process_one)(struct virtio_usb_event *event));
void virtio_usb_evt_drain_stop_cb(struct virtio_usb_queue *vq,
				  struct work_struct *work);

#endif /* VIRTIO_USB_COMMON_H */
