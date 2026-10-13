// SPDX-License-Identifier: GPL-2.0+
/*
 * virtio-snd: Virtio sound device
 * Copyright (C) 2021 OpenSynergy GmbH
 */
#include <linux/dma-mapping.h>
#include <linux/limits.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/virtio_config.h>

#include "virtio_card.h"

/**
 * struct virtio_snd_msg - Control message.
 * @sg_request: Scattergather list containing a device request (header).
 * @sg_request_data: Scattergather list containing request data.
 * @sg_response: Scattergather list containing a device response (status).
 * @sg_response_data: Scattergather list containing response data.
 * @dma_buffer: Message-owned storage for all DMA-visible SG regions.
 * @request_data_size: Size of the request data buffer.
 * @response_data_size: Size of the response data buffer.
 * @list: Pending message list entry.
 * @notify: Request completed notification.
 * @ref_count: Reference count used to manage a message lifetime.
 * @cancelled: Request canceled during device teardown.
 */
struct virtio_snd_msg {
	struct scatterlist sg_request;
	struct scatterlist sg_request_data;
	struct scatterlist sg_response;
	struct scatterlist sg_response_data;
	void *dma_buffer;
	size_t request_data_size;
	size_t response_data_size;
	struct list_head list;
	struct completion notify;
	refcount_t ref_count;
	bool cancelled;
};

/**
 * virtsnd_ctl_msg_ref() - Increment reference counter for the message.
 * @msg: Control message.
 *
 * Context: Any context.
 */
void virtsnd_ctl_msg_ref(struct virtio_snd_msg *msg)
{
	refcount_inc(&msg->ref_count);
}

/**
 * virtsnd_ctl_msg_unref() - Decrement reference counter for the message.
 * @msg: Control message.
 *
 * The message will be freed when the ref_count value is 0.
 *
 * Context: Any context.
 */
void virtsnd_ctl_msg_unref(struct virtio_snd_msg *msg)
{
	if (refcount_dec_and_test(&msg->ref_count)) {
		kfree(msg->dma_buffer);
		kfree(msg);
	}
}

/**
 * virtsnd_ctl_msg_request() - Get a pointer to the request header.
 * @msg: Control message.
 *
 * Context: Any context.
 */
void *virtsnd_ctl_msg_request(struct virtio_snd_msg *msg)
{
	return sg_virt(&msg->sg_request);
}

/**
 * virtsnd_ctl_msg_response() - Get a pointer to the response header.
 * @msg: Control message.
 *
 * Context: Any context.
 */
void *virtsnd_ctl_msg_response(struct virtio_snd_msg *msg)
{
	return sg_virt(&msg->sg_response);
}

/**
 * virtsnd_ctl_msg_request_data() - Get the request data buffer.
 * @msg: Control message.
 *
 * Context: Any context.
 */
void *virtsnd_ctl_msg_request_data(struct virtio_snd_msg *msg)
{
	return msg->request_data_size ? sg_virt(&msg->sg_request_data) : NULL;
}

/**
 * virtsnd_ctl_msg_response_data() - Get the response data buffer.
 * @msg: Control message.
 *
 * Context: Any context.
 */
void *virtsnd_ctl_msg_response_data(struct virtio_snd_msg *msg)
{
	return msg->response_data_size ? sg_virt(&msg->sg_response_data) : NULL;
}

/**
 * virtsnd_ctl_msg_sg_init() - Place one DMA region on cacheline boundaries.
 * @sg: Scattergather entry for the region.
 * @cursor: Current location in the message-owned DMA buffer.
 * @size: Region length.
 * @alignment: Alignment for DMA cachelines and typed CPU accesses.
 *
 * Context: Any context during message allocation.
 * Return: 0 on success, -EOVERFLOW if the address cannot be represented.
 */
static int virtsnd_ctl_msg_sg_init(struct scatterlist *sg, u8 **cursor,
				   size_t size, size_t alignment)
{
	uintptr_t address = (uintptr_t)*cursor;
	uintptr_t end;
	size_t padding;

	padding = address % alignment;
	if (padding &&
	    check_add_overflow(address, (uintptr_t)(alignment - padding),
			       &address))
		return -EOVERFLOW;

	if (check_add_overflow(address, (uintptr_t)size, &end))
		return -EOVERFLOW;

	padding = end % alignment;
	if (padding &&
	    check_add_overflow(end, (uintptr_t)(alignment - padding), &end))
		return -EOVERFLOW;

	sg_init_one(sg, (void *)address, (unsigned int)size);
	*cursor = (u8 *)end;

	return 0;
}

/**
 * virtsnd_ctl_msg_alloc() - Allocate and initialize a control message.
 * @request_size: Size of request header.
 * @response_size: Size of response header.
 * @request_data_size: Size of request data buffer.
 * @response_data_size: Size of response data buffer.
 * @gfp: Kernel flags for memory allocation.
 *
 * The message will be automatically freed when the ref_count value is 0.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated message on success, NULL on failure.
 */
struct virtio_snd_msg *virtsnd_ctl_msg_alloc(size_t request_size,
					     size_t response_size,
					     size_t request_data_size,
					     size_t response_data_size,
					     gfp_t gfp)
{
	struct virtio_snd_msg *msg;
	struct scatterlist *sgs[4];
	size_t sizes[4] = {
		request_size,
		response_size,
		request_data_size,
		response_data_size,
	};
	size_t alignment;
	size_t dma_buffer_size;
	size_t aligned_size;
	size_t padding;
	u8 *data;
	unsigned int i;
	int cache_alignment;
	int rc;

	if (!request_size || !response_size || request_size > UINT_MAX ||
	    response_size > UINT_MAX || request_data_size > UINT_MAX ||
	    response_data_size > UINT_MAX)
		return NULL;
	cache_alignment = dma_get_cache_alignment();
	if (cache_alignment <= 0)
		return NULL;
	/* DMA coherence does not remove the CPU's type alignment requirement. */
	alignment = max_t(size_t, cache_alignment,
			  __alignof__(struct virtio_snd_ctl_value));

	/* Reserve prefix alignment slack before the first DMA region. */
	dma_buffer_size = alignment - 1;
	for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
		if (!sizes[i])
			continue;

		padding = sizes[i] % alignment;
		aligned_size = sizes[i];
		if (padding &&
		    check_add_overflow(aligned_size, alignment - padding,
				       &aligned_size))
			return NULL;

		if (check_add_overflow(dma_buffer_size, aligned_size,
				       &dma_buffer_size))
			return NULL;
	}

	msg = kzalloc(sizeof(*msg), gfp);
	if (!msg)
		return NULL;

	msg->dma_buffer = kzalloc(dma_buffer_size, gfp);
	if (!msg->dma_buffer) {
		kfree(msg);
		return NULL;
	}
	data = msg->dma_buffer;
	sgs[0] = &msg->sg_request;
	sgs[1] = &msg->sg_response;
	sgs[2] = &msg->sg_request_data;
	sgs[3] = &msg->sg_response_data;
	for (i = 0; i < ARRAY_SIZE(sizes); ++i) {
		if (!sizes[i])
			continue;

		rc = virtsnd_ctl_msg_sg_init(sgs[i], &data, sizes[i], alignment);
		if (rc) {
			kfree(msg->dma_buffer);
			kfree(msg);
			return NULL;
		}
	}

	msg->request_data_size = request_data_size;
	msg->response_data_size = response_data_size;

	INIT_LIST_HEAD(&msg->list);
	init_completion(&msg->notify);
	/* The caller owns this reference; send adds a queue reference. */
	refcount_set(&msg->ref_count, 1);

	return msg;
}

/**
 * virtsnd_ctl_msg_send() - Send a control message.
 * @snd: VirtIO sound device.
 * @msg: Control message.
 * @nowait: Flag indicating whether to wait for completion.
 *
 * Context: Any context. Takes and releases the control queue spinlock.
 *          May sleep if @nowait is false.
 *          Consumes the caller's reference on every return path.
 * Return: 0 on success, -errno on failure.
 */
int virtsnd_ctl_msg_send(struct virtio_snd *snd, struct virtio_snd_msg *msg,
			 bool nowait)
{
	struct virtio_device *vdev = snd->vdev;
	struct virtio_snd_queue *queue = virtsnd_control_queue(snd);
	unsigned int js = msecs_to_jiffies(virtsnd_msg_timeout_ms);
	struct virtio_snd_hdr *request = virtsnd_ctl_msg_request(msg);
	struct virtio_snd_hdr *response = virtsnd_ctl_msg_response(msg);
	unsigned int nouts = 0;
	unsigned int nins = 0;
	struct scatterlist *psgs[4];
	bool notify = false;
	unsigned long flags;
	int rc;

	virtsnd_ctl_msg_ref(msg);
	/* This reference stays with the queue until completion or teardown. */

	/* Set the default status in case the message was canceled. */
	response->code = cpu_to_le32(VIRTIO_SND_S_IO_ERR);

	psgs[nouts++] = &msg->sg_request;
	if (msg->request_data_size)
		psgs[nouts++] = &msg->sg_request_data;

	psgs[nouts + nins++] = &msg->sg_response;
	if (msg->response_data_size)
		psgs[nouts + nins++] = &msg->sg_response_data;

	spin_lock_irqsave(&queue->lock, flags);
	if (snd->ctl_msgs_blocked)
		rc = -ENODEV;
	else
		rc = virtqueue_add_sgs(queue->vqueue, psgs, nouts, nins, msg,
				       GFP_ATOMIC);
	if (!rc) {
		notify = virtqueue_kick_prepare(queue->vqueue);

		list_add_tail(&msg->list, &snd->ctl_msgs);
		if (notify)
			virtqueue_notify(queue->vqueue);
	}
	spin_unlock_irqrestore(&queue->lock, flags);

	if (rc) {
		dev_err(&vdev->dev, "failed to send control message (0x%08x)\n",
			le32_to_cpu(request->code));

		/*
		 * Since in this case virtsnd_ctl_msg_complete() will not be
		 * called, it is necessary to decrement the reference count.
		 */
		virtsnd_ctl_msg_unref(msg);
	} else if (!nowait) {
		rc = wait_for_completion_interruptible_timeout(&msg->notify, js);
		if (!rc) {
			dev_err(&vdev->dev,
				"control message (0x%08x) timeout\n",
				le32_to_cpu(request->code));
			rc = -ETIMEDOUT;
		} else if (rc > 0) {
			if (READ_ONCE(msg->cancelled)) {
				rc = -ENODEV;
			} else {
				switch (le32_to_cpu(response->code)) {
				case VIRTIO_SND_S_OK:
					rc = 0;
					break;
				case VIRTIO_SND_S_NOT_SUPP:
					rc = -EOPNOTSUPP;
					break;
				case VIRTIO_SND_S_IO_ERR:
					rc = -EIO;
					break;
				default:
					rc = -EINVAL;
					break;
				}
			}
		}
	}

	/* Drop the reference consumed by this send attempt. */
	virtsnd_ctl_msg_unref(msg);

	return rc;
}

/**
 * virtsnd_ctl_msg_complete() - Complete a control message.
 * @msg: Control message.
 *
 * Context: Any context. Expects the control queue spinlock to be held by
 *          caller.
 */
void virtsnd_ctl_msg_complete(struct virtio_snd_msg *msg)
{
	list_del(&msg->list);
	complete(&msg->notify);

	virtsnd_ctl_msg_unref(msg);
}

/**
 * virtsnd_ctl_msg_cancel_all() - Cancel all pending control messages.
 * @snd: VirtIO sound device.
 *
 * Context: Any context. Blocks new senders and marks pending requests canceled.
 *          Existing senders are woken when their request completes or after
 *          the device has been reset.
 */
void virtsnd_ctl_msg_cancel_all(struct virtio_snd *snd)
{
	struct virtio_snd_queue *queue = virtsnd_control_queue(snd);
	struct virtio_snd_msg *msg;

	guard(spinlock_irqsave)(&queue->lock);
	snd->ctl_msgs_blocked = true;
	list_for_each_entry(msg, &snd->ctl_msgs, list) {
		WRITE_ONCE(msg->cancelled, true);
	}
}

/**
 * virtsnd_ctl_msg_cleanup() - Release messages left after queue teardown.
 * @snd: VirtIO sound device.
 *
 * This must be called after device reset, buffer detachment, and del_vqs(),
 * when no queue callback or DMA access can still reach a message. The queue
 * object and its lock must remain alive until this function returns.
 *
 * Context: Any context.
 */
void virtsnd_ctl_msg_cleanup(struct virtio_snd *snd)
{
	struct virtio_snd_queue *queue = virtsnd_control_queue(snd);

	guard(spinlock_irqsave)(&queue->lock);
	while (!list_empty(&snd->ctl_msgs)) {
		struct virtio_snd_msg *msg =
			list_first_entry(&snd->ctl_msgs, struct virtio_snd_msg,
					 list);

		virtsnd_ctl_msg_complete(msg);
	}
}

/**
 * virtsnd_ctl_msg_enable() - Allow control requests after queue restoration.
 * @snd: VirtIO sound device.
 *
 * Context: Any context.
 */
void virtsnd_ctl_msg_enable(struct virtio_snd *snd)
{
	struct virtio_snd_queue *queue = virtsnd_control_queue(snd);

	guard(spinlock_irqsave)(&queue->lock);
	snd->ctl_msgs_blocked = false;
}

/**
 * virtsnd_ctl_query_info() - Query the item configuration from the device.
 * @snd: VirtIO sound device.
 * @command: Control request code (VIRTIO_SND_R_XXX_INFO).
 * @start_id: Item start identifier.
 * @count: Item count to query.
 * @size: Item information size in bytes.
 * @info: Buffer for storing item information.
 *
 * Context: Any context that permits to sleep.
 * Return: 0 on success, -errno on failure.
 */
int virtsnd_ctl_query_info(struct virtio_snd *snd, int command, int start_id,
			   int count, size_t size, void *info)
{
	struct virtio_snd_msg *msg;
	struct virtio_snd_query_info *query;
	size_t info_size;
	int rc;

	if (count < 0)
		return -EINVAL;

	if (size > UINT_MAX ||
	    check_mul_overflow((size_t)count, size, &info_size) ||
	    info_size > UINT_MAX)
		return -EOVERFLOW;

	if (info_size && !info)
		return -EINVAL;

	msg = virtsnd_ctl_msg_alloc(sizeof(*query),
				    sizeof(struct virtio_snd_hdr), 0, info_size,
				    GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	query = virtsnd_ctl_msg_request(msg);
	query->hdr.code = cpu_to_le32(command);
	query->start_id = cpu_to_le32(start_id);
	query->count = cpu_to_le32(count);
	query->size = cpu_to_le32(size);

	virtsnd_ctl_msg_ref(msg);
	rc = virtsnd_ctl_msg_send(snd, msg, false);
	if (!rc && info_size)
		memcpy(info, virtsnd_ctl_msg_response_data(msg), info_size);
	virtsnd_ctl_msg_unref(msg);

	return rc;
}

/**
 * virtsnd_ctl_notify_cb() - Process all completed control messages.
 * @vqueue: Underlying control virtqueue.
 *
 * This callback function is called upon a vring interrupt request from the
 * device.
 *
 * Context: Interrupt context. Takes and releases the control queue spinlock.
 */
void virtsnd_ctl_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_snd *snd = vqueue->vdev->priv;
	struct virtio_snd_queue *queue = virtsnd_control_queue(snd);
	struct virtio_snd_msg *msg;
	u32 length;

	guard(spinlock_irqsave)(&queue->lock);
	do {
		virtqueue_disable_cb(vqueue);
		while ((msg = virtqueue_get_buf(vqueue, &length)))
			virtsnd_ctl_msg_complete(msg);
	} while (!virtqueue_enable_cb(vqueue));
}
