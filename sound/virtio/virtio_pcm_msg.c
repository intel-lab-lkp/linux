// SPDX-License-Identifier: GPL-2.0+
/*
 * virtio-snd: Virtio sound device
 * Copyright (C) 2021 OpenSynergy GmbH
 */
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <sound/pcm_params.h>

#include "virtio_card.h"

/**
 * struct virtio_pcm_msg - VirtIO I/O message.
 * @substream: VirtIO PCM substream.
 * @xfer: Request header payload.
 * @status: Response header payload.
 * @length: Data length in bytes.
 * @data_offset: Offset in the driver-owned PCM buffer on NOMMU systems.
 * @sgs: Payload scatter-gather table.
 */
struct virtio_pcm_msg {
	struct virtio_pcm_substream *substream;
	struct virtio_snd_pcm_xfer xfer;
	struct virtio_snd_pcm_status status;
	size_t length;
#ifndef CONFIG_MMU
	size_t data_offset;
#endif
	struct scatterlist sgs[];
};

/**
 * enum pcm_msg_sg_index - Index values for the virtio_pcm_msg->sgs field in
 *                         an I/O message.
 * @PCM_MSG_SG_XFER: Element containing a virtio_snd_pcm_xfer structure.
 * @PCM_MSG_SG_STATUS: Element containing a virtio_snd_pcm_status structure.
 * @PCM_MSG_SG_DATA: The first element containing a data buffer.
 */
enum pcm_msg_sg_index {
	PCM_MSG_SG_XFER = 0,
	PCM_MSG_SG_STATUS,
	PCM_MSG_SG_DATA
};

/**
 * virtsnd_pcm_sg_num() - Count the sg-elements for a page-backed buffer.
 * @data: Pointer to the page-backed buffer.
 * @length: Buffer size.
 *
 * Context: Any context.
 * Return: Number of physically contiguous parts in the @data.
 */
static int virtsnd_pcm_sg_num(u8 *data, unsigned int length)
{
	phys_addr_t sg_address = 0;
	unsigned int sg_length = 0;
	int num = 0;

	while (length) {
		struct page *pg = vmalloc_to_page(data);
		phys_addr_t pg_address;
		size_t pg_length;

		if (!pg)
			return -EINVAL;
		pg_address = page_to_phys(pg);

		pg_length = PAGE_SIZE - offset_in_page(data);
		if (pg_length > length)
			pg_length = length;

		if (!num || sg_address + sg_length != pg_address) {
			sg_address = pg_address;
			sg_length = pg_length;
			num++;
		} else {
			sg_length += pg_length;
		}

		data += pg_length;
		length -= pg_length;
	}

	return num;
}

#ifdef CONFIG_MMU
/**
 * virtsnd_pcm_dma_pages_put() - Release retained PCM buffer pages.
 * @vss: VirtIO PCM substream.
 *
 * Context: Process context.
 */
static void virtsnd_pcm_dma_pages_put(struct virtio_pcm_substream *vss)
{
	while (vss->ndma_pages)
		put_page(vss->dma_pages[--vss->ndma_pages]);
	kfree(vss->dma_pages);
	vss->dma_pages = NULL;
}

/**
 * virtsnd_pcm_dma_pages_get() - Retain the pages backing the PCM buffer.
 * @vss: VirtIO PCM substream.
 * @data: PCM buffer address.
 * @length: PCM buffer size.
 *
 * Keep the backing pages alive while virtqueue descriptors can still refer to
 * them, even if ALSA releases the vmalloc mapping after a failed drain.
 *
 * Context: Process context.
 * Return: 0 on success, -errno on failure.
 */
static int virtsnd_pcm_dma_pages_get(struct virtio_pcm_substream *vss,
				     u8 *data, size_t length)
{
	size_t i;
	size_t npages;
	size_t span;

	if (!data || !length || !is_vmalloc_addr(data))
		return -EINVAL;
	if (vss->dma_pages || vss->ndma_pages)
		return -EBUSY;

	if (check_add_overflow(length, (size_t)offset_in_page(data), &span))
		return -EOVERFLOW;

	npages = span / PAGE_SIZE + !!(span % PAGE_SIZE);
	if (npages > UINT_MAX)
		return -EOVERFLOW;

	vss->dma_pages = kcalloc(npages, sizeof(*vss->dma_pages), GFP_KERNEL);
	if (!vss->dma_pages)
		return -ENOMEM;

	for (i = 0; i < npages; i++) {
		struct page *page = vmalloc_to_page(data + i * PAGE_SIZE);

		if (!page)
			goto err_put_pages;

		get_page(page);
		vss->dma_pages[vss->ndma_pages++] = page;
	}

	return 0;

err_put_pages:
	virtsnd_pcm_dma_pages_put(vss);

	return -EINVAL;
}
#endif

/**
 * virtsnd_pcm_sg_from() - Build an sg-list from a page-backed buffer.
 * @sgs: Preallocated sg-list to populate.
 * @nsgs: The maximum number of elements in the @sgs.
 * @data: Pointer to the page-backed buffer.
 * @length: Buffer size.
 *
 * Splits the buffer into physically contiguous parts and makes an sg-list of
 * such parts.
 *
 * Context: Any context.
 */
static void virtsnd_pcm_sg_from(struct scatterlist *sgs, int nsgs, u8 *data,
				unsigned int length)
{
	int idx = -1;

	while (length) {
		struct page *pg = vmalloc_to_page(data);
		size_t pg_length;

		pg_length = PAGE_SIZE - offset_in_page(data);
		if (pg_length > length)
			pg_length = length;

		if (idx == -1 ||
		    sg_phys(&sgs[idx]) + sgs[idx].length != page_to_phys(pg)) {
			if (idx + 1 == nsgs)
				break;
			sg_set_page(&sgs[++idx], pg, pg_length,
				    offset_in_page(data));
		} else {
			sgs[idx].length += pg_length;
		}

		data += pg_length;
		length -= pg_length;
	}

	sg_mark_end(&sgs[idx]);
}

/**
 * virtsnd_pcm_msg_alloc() - Allocate I/O messages.
 * @vss: VirtIO PCM substream.
 * @periods: Current number of periods.
 * @period_bytes: Current period size in bytes.
 *
 * The function slices the buffer into @periods parts (each with the size of
 * @period_bytes), and creates @periods corresponding I/O messages.
 *
 * Context: Any context that permits to sleep.
 * Return: 0 on success, -ENOMEM on failure.
 */
int virtsnd_pcm_msg_alloc(struct virtio_pcm_substream *vss,
			  unsigned int periods, unsigned int period_bytes)
{
	struct snd_pcm_runtime *runtime;
	size_t buffer_bytes;
	unsigned int i;
	int rc;

	if (vss->msgs || vss->nmsgs || vss->dma_pages || vss->ndma_pages)
		return -EBUSY;
#ifndef CONFIG_MMU
	if (vss->owned_dma_area || vss->owned_dma_bytes)
		return -EBUSY;
#endif
	if (!periods || !period_bytes)
		return -EINVAL;

	if (check_mul_overflow((size_t)periods, (size_t)period_bytes,
			       &buffer_bytes))
		return -EOVERFLOW;

	if (!vss->substream || !vss->substream->runtime)
		return -EINVAL;
	runtime = vss->substream->runtime;
	if (!runtime->dma_area || buffer_bytes > runtime->dma_bytes)
		return -EINVAL;

#ifdef CONFIG_MMU
	rc = virtsnd_pcm_dma_pages_get(vss, runtime->dma_area, buffer_bytes);
	if (rc)
		return rc;
#else
	vss->owned_dma_area = kzalloc(buffer_bytes, GFP_KERNEL);
	if (!vss->owned_dma_area)
		return -ENOMEM;
	vss->owned_dma_bytes = buffer_bytes;
#endif

	vss->msgs = kzalloc_objs(*vss->msgs, periods);
	if (!vss->msgs) {
		rc = -ENOMEM;
		goto err_free_pages;
	}

	vss->nmsgs = periods;

	for (i = 0; i < periods; ++i) {
		int sg_num;
		struct virtio_pcm_msg *msg;
		u8 *data;
#ifdef CONFIG_MMU
		data = runtime->dma_area + (size_t)period_bytes * i;
#else
		data = vss->owned_dma_area + (size_t)period_bytes * i;
#endif
		sg_num = virtsnd_pcm_sg_num(data, period_bytes);
		if (sg_num < 0) {
			rc = sg_num;
			goto err_free_msgs;
		}

		msg = kzalloc_flex(*msg, sgs, sg_num + 2);
		if (!msg) {
			rc = -ENOMEM;
			goto err_free_msgs;
		}

		msg->substream = vss;
		vss->msgs[i] = msg;
		sg_init_one(&msg->sgs[PCM_MSG_SG_XFER], &msg->xfer,
			    sizeof(msg->xfer));
		sg_init_one(&msg->sgs[PCM_MSG_SG_STATUS], &msg->status,
			    sizeof(msg->status));
#ifndef CONFIG_MMU
		msg->data_offset = (size_t)period_bytes * i;
#endif
		virtsnd_pcm_sg_from(&msg->sgs[PCM_MSG_SG_DATA], sg_num, data,
				    period_bytes);
	}

	return 0;

err_free_msgs:
	virtsnd_pcm_msg_free(vss);
	return rc;

err_free_pages:
#ifdef CONFIG_MMU
	virtsnd_pcm_dma_pages_put(vss);
#else
	kfree(vss->owned_dma_area);
	vss->owned_dma_area = NULL;
	vss->owned_dma_bytes = 0;
#endif

	return rc;
}

/**
 * virtsnd_pcm_msg_free() - Free all allocated I/O messages.
 * @vss: VirtIO PCM substream.
 *
 * Context: Any context.
 */
void virtsnd_pcm_msg_free(struct virtio_pcm_substream *vss)
{
	unsigned int i;

	for (i = 0; vss->msgs && i < vss->nmsgs; ++i)
		kfree(vss->msgs[i]);
	kfree(vss->msgs);
#ifdef CONFIG_MMU
	virtsnd_pcm_dma_pages_put(vss);
#else
	kfree(vss->owned_dma_area);
	vss->owned_dma_area = NULL;
	vss->owned_dma_bytes = 0;
#endif

	vss->msgs = NULL;
	vss->nmsgs = 0;
}

/**
 * virtsnd_pcm_msg_reset() - Reset the fill level of all I/O messages.
 * @vss: VirtIO PCM substream.
 *
 * Messages that were only partially filled by the application when the
 * substream was stopped are never sent to the device, so their length must be
 * cleared before the substream is started again.
 *
 * Context: Any context. Takes and releases the VirtIO substream spinlock.
 */
void virtsnd_pcm_msg_reset(struct virtio_pcm_substream *vss)
{
	unsigned int i;

	guard(spinlock_irqsave)(&vss->lock);
	for (i = 0; vss->msgs && i < vss->nmsgs; ++i)
		vss->msgs[i]->length = 0;
}

/**
 * virtsnd_pcm_msg_send() - Send asynchronous I/O messages.
 * @vss: VirtIO PCM substream.
 * @offset: starting position that has been updated
 * @bytes: number of bytes that has been updated
 *
 * All messages are organized in an ordered circular list. Each time the
 * function is called, all currently non-enqueued messages are added to the
 * virtqueue. For this, the function uses offset and bytes to calculate the
 * messages that need to be added.
 *
 * Context: Any context. Expects the tx/rx queue and the VirtIO substream
 *          spinlocks to be held by caller.
 * Return: 0 on success, -errno on failure.
 */
int virtsnd_pcm_msg_send(struct virtio_pcm_substream *vss, unsigned long offset,
			 unsigned long bytes)
{
	struct virtio_snd *snd = vss->snd;
	struct virtio_device *vdev = snd->vdev;
	struct virtqueue *vqueue = virtsnd_pcm_queue(vss)->vqueue;
	unsigned long period_bytes = snd_pcm_lib_period_bytes(vss->substream);
	unsigned long start, end, i;
	unsigned int msg_count = vss->msg_count;
	bool notify = false;
	int rc;

	start = offset / period_bytes;
	end = (offset + bytes - 1) / period_bytes;

	for (i = start; i <= end; i++) {
		struct virtio_pcm_msg *msg = vss->msgs[i];
		struct scatterlist *psgs[] = {
			&msg->sgs[PCM_MSG_SG_XFER],
			&msg->sgs[PCM_MSG_SG_DATA],
			&msg->sgs[PCM_MSG_SG_STATUS]
		};
		unsigned long n;

		n = period_bytes - (offset % period_bytes);
		if (n > bytes)
			n = bytes;

		msg->length += n;
		if (msg->length == period_bytes) {
			msg->xfer.stream_id = cpu_to_le32(vss->sid);
			memset(&msg->status, 0, sizeof(msg->status));

#ifndef CONFIG_MMU
		if (vss->direction == SNDRV_PCM_STREAM_PLAYBACK) {
			struct snd_pcm_runtime *runtime =
				vss->substream ? vss->substream->runtime : NULL;
			size_t data_bytes = msg->length;

			if (!runtime || !runtime->dma_area || !vss->owned_dma_area ||
			    msg->data_offset > runtime->dma_bytes ||
			    data_bytes > runtime->dma_bytes - msg->data_offset ||
			    msg->data_offset > vss->owned_dma_bytes ||
			    data_bytes > vss->owned_dma_bytes - msg->data_offset) {
				dev_err_ratelimited(&vdev->dev,
						    "SID %u: invalid playback buffer for I/O message\n",
					vss->sid);
				msg->length -= n;
				if (msg_count != vss->msg_count &&
				    !(vss->features &
				      (1U << VIRTIO_SND_PCM_F_MSG_POLLING)) &&
				    virtqueue_kick_prepare(vqueue))
					virtqueue_notify(vqueue);
				return -EINVAL;
			}
			memcpy(vss->owned_dma_area + msg->data_offset,
			       runtime->dma_area + msg->data_offset, data_bytes);
		}
#endif

			if (vss->direction == SNDRV_PCM_STREAM_PLAYBACK)
				rc = virtqueue_add_sgs(vqueue, psgs, 2, 1, msg,
						       GFP_ATOMIC);
			else
				rc = virtqueue_add_sgs(vqueue, psgs, 1, 2, msg,
						       GFP_ATOMIC);

			if (rc) {
				dev_err(&vdev->dev,
					"SID %u: failed to send I/O message\n",
					vss->sid);
				msg->length -= n;

				/*
				 * Do not leave earlier messages queued without notifying
				 * the device when a later message could not be added.
				 */
				if (msg_count != vss->msg_count &&
				    !(vss->features &
				      (1U << VIRTIO_SND_PCM_F_MSG_POLLING)) &&
				    virtqueue_kick_prepare(vqueue))
					virtqueue_notify(vqueue);

				return rc;
			}

			vss->msg_count++;
		}

		offset = 0;
		bytes -= n;
	}

	if (msg_count == vss->msg_count)
		return 0;

	if (!(vss->features & (1U << VIRTIO_SND_PCM_F_MSG_POLLING)))
		notify = virtqueue_kick_prepare(vqueue);

	if (notify)
		virtqueue_notify(vqueue);

	return 0;
}

/**
 * virtsnd_pcm_msg_pending_num() - Returns the number of pending I/O messages.
 * @vss: VirtIO substream.
 *
 * Context: Any context.
 * Return: Number of messages.
 */
unsigned int virtsnd_pcm_msg_pending_num(struct virtio_pcm_substream *vss)
{
	guard(spinlock_irqsave)(&vss->lock);
	return vss->msg_count;
}

/**
 * virtsnd_pcm_msg_cancel() - Cancel a message detached during device reset.
 * @msg: Message returned by virtqueue_detach_unused_buf().
 *
 * Reset cancellation retires the queue token without treating it as a device
 * completion. In particular, it does not advance the PCM hardware pointer or
 * schedule period work.
 *
 * Context: Process context with the PCM queue lock held.
 */
void virtsnd_pcm_msg_cancel(struct virtio_pcm_msg *msg)
{
	struct virtio_pcm_substream *vss = msg->substream;
	bool wake;

	{
		guard(spinlock)(&vss->lock);
		msg->length = 0;
		vss->xfer_enabled = false;
		if (WARN_ON_ONCE(!vss->msg_count))
			return;
		vss->msg_count--;
		wake = !vss->msg_count;
	}

	if (wake)
		wake_up_all(&vss->msg_empty);
}

/**
 * virtsnd_pcm_msg_complete() - Complete an I/O message.
 * @msg: I/O message.
 * @written_bytes: Number of bytes written to the message.
 *
 * Completion of the message means the elapsed period. If transmission is
 * allowed, then each completed message is immediately placed back at the end
 * of the queue.
 *
 * For the playback substream, @written_bytes is equal to sizeof(msg->status).
 *
 * For the capture substream, @written_bytes is equal to sizeof(msg->status)
 * plus the number of captured bytes.
 *
 * Context: Interrupt context. Takes and releases the VirtIO substream spinlock.
 */
static void virtsnd_pcm_msg_complete(struct virtio_pcm_msg *msg,
				     size_t written_bytes)
{
	struct virtio_pcm_substream *vss = msg->substream;

	/*
	 * hw_ptr always indicates the buffer position of the first I/O message
	 * in the virtqueue. Therefore, on each completion of an I/O message,
	 * the hw_ptr value is unconditionally advanced.
	 */
	guard(spinlock)(&vss->lock);
#ifndef CONFIG_MMU
	if (vss->direction == SNDRV_PCM_STREAM_CAPTURE &&
	    vss->xfer_enabled && written_bytes > sizeof(msg->status)) {
		struct snd_pcm_runtime *runtime =
			vss->substream ? vss->substream->runtime : NULL;
		size_t reported_bytes = written_bytes - sizeof(msg->status);
		size_t data_bytes = min_t(size_t, reported_bytes, msg->length);

		if (!runtime || !runtime->dma_area || !vss->owned_dma_area ||
		    msg->data_offset > runtime->dma_bytes ||
		    data_bytes > runtime->dma_bytes - msg->data_offset ||
		    msg->data_offset > vss->owned_dma_bytes ||
		    data_bytes > vss->owned_dma_bytes - msg->data_offset)
			dev_err_ratelimited(&vss->snd->vdev->dev,
					    "SID %u: invalid capture buffer for I/O message\n",
				vss->sid);
		else
			memcpy(runtime->dma_area + msg->data_offset,
			       vss->owned_dma_area + msg->data_offset, data_bytes);
	}
#endif
	if (vss->direction == SNDRV_PCM_STREAM_CAPTURE &&
	    written_bytes > sizeof(msg->status) &&
	    written_bytes - sizeof(msg->status) > msg->length)
		dev_err_ratelimited(&vss->snd->vdev->dev,
				    "SID %u: capture length exceeds I/O message capacity\n",
			vss->sid);
	/*
	 * If the capture substream returned an incorrect status, then just
	 * increase the hw_ptr by the message size. Clamp valid capture lengths to
	 * the actual SG payload capacity before accounting them.
	 */
	if (vss->direction == SNDRV_PCM_STREAM_PLAYBACK ||
	    written_bytes <= sizeof(msg->status)) {
		vss->hw_ptr += msg->length;
	} else {
		size_t data_bytes = min_t(size_t,
					  written_bytes - sizeof(msg->status),
					  msg->length);

		vss->hw_ptr += data_bytes;
	}

	if (vss->hw_ptr >= vss->buffer_bytes)
		vss->hw_ptr -= vss->buffer_bytes;

	msg->length = 0;

	vss->xfer_xrun = false;
	vss->msg_count--;

	if (vss->xfer_enabled) {
		struct snd_pcm_runtime *runtime = vss->substream->runtime;

		runtime->delay =
			bytes_to_frames(runtime,
					le32_to_cpu(msg->status.latency_bytes));
		schedule_work(&vss->elapsed_period);
	} else if (!vss->msg_count) {
		wake_up_all(&vss->msg_empty);
	}
}

/**
 * virtsnd_pcm_notify_cb() - Process all completed I/O messages.
 * @queue: Underlying tx/rx virtqueue.
 *
 * Context: Interrupt context. Takes and releases the tx/rx queue spinlock.
 */
static inline void virtsnd_pcm_notify_cb(struct virtio_snd_queue *queue)
{
	struct virtio_pcm_msg *msg;
	u32 written_bytes;

	guard(spinlock_irqsave)(&queue->lock);
	do {
		virtqueue_disable_cb(queue->vqueue);
		while ((msg = virtqueue_get_buf(queue->vqueue, &written_bytes)))
			virtsnd_pcm_msg_complete(msg, written_bytes);
	} while (!virtqueue_enable_cb(queue->vqueue));
}

/**
 * virtsnd_pcm_tx_notify_cb() - Process all completed TX messages.
 * @vqueue: Underlying tx virtqueue.
 *
 * Context: Interrupt context.
 */
void virtsnd_pcm_tx_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_snd *snd = vqueue->vdev->priv;

	virtsnd_pcm_notify_cb(virtsnd_tx_queue(snd));
}

/**
 * virtsnd_pcm_rx_notify_cb() - Process all completed RX messages.
 * @vqueue: Underlying rx virtqueue.
 *
 * Context: Interrupt context.
 */
void virtsnd_pcm_rx_notify_cb(struct virtqueue *vqueue)
{
	struct virtio_snd *snd = vqueue->vdev->priv;

	virtsnd_pcm_notify_cb(virtsnd_rx_queue(snd));
}

/**
 * virtsnd_pcm_ctl_msg_alloc() - Allocate and initialize the PCM device control
 *                               message for the specified substream.
 * @vss: VirtIO PCM substream.
 * @command: Control request code (VIRTIO_SND_R_PCM_XXX).
 * @gfp: Kernel flags for memory allocation.
 *
 * Context: Any context. May sleep if @gfp flags permit.
 * Return: Allocated message on success, NULL on failure.
 */
struct virtio_snd_msg *
virtsnd_pcm_ctl_msg_alloc(struct virtio_pcm_substream *vss,
			  unsigned int command, gfp_t gfp)
{
	size_t request_size = sizeof(struct virtio_snd_pcm_hdr);
	size_t response_size = sizeof(struct virtio_snd_hdr);
	struct virtio_snd_msg *msg;

	switch (command) {
	case VIRTIO_SND_R_PCM_SET_PARAMS:
		request_size = sizeof(struct virtio_snd_pcm_set_params);
		break;
	}

	msg = virtsnd_ctl_msg_alloc(request_size, response_size, 0, 0, gfp);
	if (msg) {
		struct virtio_snd_pcm_hdr *hdr = virtsnd_ctl_msg_request(msg);

		hdr->hdr.code = cpu_to_le32(command);
		hdr->stream_id = cpu_to_le32(vss->sid);
	}

	return msg;
}
