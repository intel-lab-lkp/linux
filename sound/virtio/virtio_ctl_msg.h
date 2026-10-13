/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * virtio-snd: Virtio sound device
 * Copyright (C) 2021 OpenSynergy GmbH
 */
#ifndef VIRTIO_SND_MSG_H
#define VIRTIO_SND_MSG_H

#include <linux/atomic.h>
#include <linux/virtio.h>

struct virtio_snd;
struct virtio_snd_msg;

void virtsnd_ctl_msg_ref(struct virtio_snd_msg *msg);

void virtsnd_ctl_msg_unref(struct virtio_snd_msg *msg);

void *virtsnd_ctl_msg_request(struct virtio_snd_msg *msg);

void *virtsnd_ctl_msg_response(struct virtio_snd_msg *msg);

struct virtio_snd_msg *virtsnd_ctl_msg_alloc(size_t request_size,
					     size_t response_size,
					     size_t request_data_size,
					     size_t response_data_size,
					     gfp_t gfp);

void *virtsnd_ctl_msg_request_data(struct virtio_snd_msg *msg);

void *virtsnd_ctl_msg_response_data(struct virtio_snd_msg *msg);

int virtsnd_ctl_msg_send(struct virtio_snd *snd, struct virtio_snd_msg *msg,
			 bool nowait);

/**
 * virtsnd_ctl_msg_send_sync() - Simplified sending of synchronous message.
 * @snd: VirtIO sound device.
 * @msg: Control message.
 *
 * This function consumes the caller's message reference on return. A message
 * accepted by the control queue keeps its queue reference until completion or
 * post-reset cleanup, including when this function returns on timeout or
 * interruption. Callers that read the response after success must retain an
 * additional reference through that access.
 *
 * The msg_timeout_ms module parameter defines the message completion timeout.
 * If the message is not completed within this time, the function will return an
 * error.
 *
 * Context: Any context that permits to sleep.
 * Return: 0 on success, -errno on failure.
 *
 * The return value is a message status code (VIRTIO_SND_S_XXX) converted to an
 * appropriate -errno value.
 */
static inline int virtsnd_ctl_msg_send_sync(struct virtio_snd *snd,
					    struct virtio_snd_msg *msg)
{
	return virtsnd_ctl_msg_send(snd, msg, false);
}

/**
 * virtsnd_ctl_msg_send_async() - Simplified sending of asynchronous message.
 * @snd: VirtIO sound device.
 * @msg: Control message.
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static inline int virtsnd_ctl_msg_send_async(struct virtio_snd *snd,
					     struct virtio_snd_msg *msg)
{
	return virtsnd_ctl_msg_send(snd, msg, true);
}

void virtsnd_ctl_msg_cancel_all(struct virtio_snd *snd);

void virtsnd_ctl_msg_cleanup(struct virtio_snd *snd);

void virtsnd_ctl_msg_enable(struct virtio_snd *snd);

void virtsnd_ctl_msg_complete(struct virtio_snd_msg *msg);

int virtsnd_ctl_query_info(struct virtio_snd *snd, int command, int start_id,
			   int count, size_t size, void *info);

void virtsnd_ctl_notify_cb(struct virtqueue *vqueue);

#endif /* VIRTIO_SND_MSG_H */
