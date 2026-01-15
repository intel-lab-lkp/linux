// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025, Silicon Laboratories, Inc.
 */

#include <linux/err.h>
#include <linux/greybus.h>
#include <linux/module.h>
#include <linux/skbuff.h>

#include "cpc.h"
#include "header.h"
#include "host.h"

static struct cpc_host_device *gb_hd_to_cpc_hd(struct gb_host_device *hd)
{
	return (struct cpc_host_device *)&hd->hd_priv;
}

static struct cpc_cport *cpc_hd_get_cport(struct cpc_host_device *cpc_hd, u16 cport_id)
{
	struct cpc_cport *cport;

	mutex_lock(&cpc_hd->lock);
	for (int i = 0; i < ARRAY_SIZE(cpc_hd->cports); i++) {
		cport = cpc_hd->cports[i];
		if (cport && cport->id == cport_id)
			goto unlock;
	}

	cport = NULL;

unlock:
	mutex_unlock(&cpc_hd->lock);

	return cport;
}

static int cpc_hd_message_send(struct cpc_host_device *cpc_hd, u16 cport_id,
			       struct gb_message *message, gfp_t gfp_mask)
{
	struct cpc_cport *cport;
	struct sk_buff *skb;
	unsigned int size;

	cport = cpc_hd_get_cport(cpc_hd, cport_id);
	if (!cport) {
		dev_err(cpc_hd_dev(cpc_hd), "message_send: cport %u not found\n", cport_id);
		return -EINVAL;
	}

	size = sizeof(*message->header) + message->payload_size + CPC_HEADER_SIZE;
	skb = alloc_skb(size, gfp_mask);
	if (!skb)
		return -ENOMEM;

	skb_reserve(skb, CPC_HEADER_SIZE);

	/* Header and payload are already contiguous in Greybus message */
	skb_put_data(skb, message->buffer, sizeof(*message->header) + message->payload_size);

	CPC_SKB_CB(skb)->cport = cport;
	CPC_SKB_CB(skb)->gb_message = message;

	cpc_cport_transmit(cport, skb);

	return 0;
}

static int cpc_hd_cport_allocate(struct cpc_host_device *cpc_hd, int cport_id, unsigned long flags)
{
	struct cpc_cport *cport;
	int ret;

	mutex_lock(&cpc_hd->lock);
	for (int i = 0; i < ARRAY_SIZE(cpc_hd->cports); i++) {
		if (cpc_hd->cports[i] != NULL)
			continue;

		if (cport_id < 0)
			cport_id = i;

		cport = cpc_cport_alloc(cport_id, GFP_KERNEL);
		if (!cport) {
			ret = -ENOMEM;
			goto unlock;
		}

		cport->cpc_hd = cpc_hd;

		cpc_hd->cports[i] = cport;
		ret = cport_id;
		goto unlock;
	}

	ret = -ENOSPC;
unlock:
	mutex_unlock(&cpc_hd->lock);

	return ret;
}

static void cpc_hd_cport_release(struct cpc_host_device *cpc_hd, u16 cport_id)
{
	struct cpc_cport *cport;

	mutex_lock(&cpc_hd->lock);
	for (int i = 0; i < ARRAY_SIZE(cpc_hd->cports); i++) {
		cport = cpc_hd->cports[i];

		if (cport && cport->id == cport_id) {
			cpc_cport_release(cport);
			cpc_hd->cports[i] = NULL;
			break;
		}
	}
	mutex_unlock(&cpc_hd->lock);
}

static int cpc_gb_message_send(struct gb_host_device *gb_hd, u16 cport_id,
			       struct gb_message *message, gfp_t gfp_mask)
{
	struct cpc_host_device *cpc_hd = gb_hd_to_cpc_hd(gb_hd);

	return cpc_hd_message_send(cpc_hd, cport_id, message, gfp_mask);
}

static void cpc_gb_message_cancel(struct gb_message *message)
{
	/* Not implemented */
}

static int cpc_gb_cport_allocate(struct gb_host_device *gb_hd, int cport_id, unsigned long flags)
{
	struct cpc_host_device *cpc_hd = gb_hd_to_cpc_hd(gb_hd);

	return cpc_hd_cport_allocate(cpc_hd, cport_id, flags);
}

static void cpc_gb_cport_release(struct gb_host_device *gb_hd, u16 cport_id)
{
	struct cpc_host_device *cpc_hd = gb_hd_to_cpc_hd(gb_hd);

	return cpc_hd_cport_release(cpc_hd, cport_id);
}

static struct gb_hd_driver cpc_gb_driver = {
	.hd_priv_size = sizeof(struct cpc_host_device),
	.message_send = cpc_gb_message_send,
	.message_cancel = cpc_gb_message_cancel,
	.cport_allocate = cpc_gb_cport_allocate,
	.cport_release = cpc_gb_cport_release,
};

static void cpc_hd_init(struct cpc_host_device *cpc_hd)
{
	mutex_init(&cpc_hd->lock);
	skb_queue_head_init(&cpc_hd->tx_queue);
}

struct cpc_host_device *cpc_hd_create(struct cpc_hd_driver *driver, struct device *parent)
{
	struct cpc_host_device *cpc_hd;
	struct gb_host_device *hd;

	if (!driver->wake_tx) {
		dev_err(parent, "missing mandatory callback\n");
		return ERR_PTR(-EINVAL);
	}

	hd = gb_hd_create(&cpc_gb_driver, parent, GB_CPC_MSG_SIZE_MAX, GB_CPC_NUM_CPORTS);
	if (IS_ERR(hd))
		return (struct cpc_host_device *)hd;

	cpc_hd = gb_hd_to_cpc_hd(hd);
	cpc_hd->gb_hd = hd;
	cpc_hd->driver = driver;

	cpc_hd_init(cpc_hd);

	return cpc_hd;
}
EXPORT_SYMBOL_GPL(cpc_hd_create);

int cpc_hd_add(struct cpc_host_device *cpc_hd)
{
	return gb_hd_add(cpc_hd->gb_hd);
}
EXPORT_SYMBOL_GPL(cpc_hd_add);

void cpc_hd_put(struct cpc_host_device *cpc_hd)
{
	return gb_hd_put(cpc_hd->gb_hd);
}
EXPORT_SYMBOL_GPL(cpc_hd_put);

void cpc_hd_del(struct cpc_host_device *cpc_hd)
{
	return gb_hd_del(cpc_hd->gb_hd);
}
EXPORT_SYMBOL_GPL(cpc_hd_del);

void cpc_hd_rcvd(struct cpc_host_device *cpc_hd, struct sk_buff *skb)
{
	struct gb_operation_msg_hdr *gb_hdr;
	struct cpc_cport *cport;
	u16 cport_id;

	/* Prevent an out-of-bound access if called with non-sensical parameters. */
	if (skb->len < (sizeof(*gb_hdr) + CPC_HEADER_SIZE))
		goto free_skb;

	/* Retrieve cport ID that was packed in Greybus header */
	gb_hdr = (struct gb_operation_msg_hdr *)(skb->data + CPC_HEADER_SIZE);
	cport_id = cpc_cport_unpack(gb_hdr);

	cport = cpc_hd_get_cport(cpc_hd, cport_id);
	if (!cport) {
		dev_warn(cpc_hd_dev(cpc_hd), "cport %u not allocated\n", cport_id);
		goto free_skb;
	}

	cpc_protocol_on_data(cport, skb);

free_skb:
	kfree_skb(skb);
}
EXPORT_SYMBOL_GPL(cpc_hd_rcvd);

/**
 * cpc_hd_send_skb() - Queue a socket buffer for transmission.
 * @cpc_hd: Host device to send SKB over.
 * @skb: SKB to send.
 */
void cpc_hd_send_skb(struct cpc_host_device *cpc_hd, struct sk_buff *skb)
{
	const struct cpc_hd_driver *drv = cpc_hd->driver;

	mutex_lock(&cpc_hd->lock);
	skb_queue_tail(&cpc_hd->tx_queue, skb);
	mutex_unlock(&cpc_hd->lock);

	drv->wake_tx(cpc_hd);
}

/**
 * cpc_hd_tx_queue_empty() - Check if transmit queue is empty.
 * @cpc_hd: CPC Host Device.
 *
 * Return: True if transmit queue is empty, false otherwise.
 */
bool cpc_hd_tx_queue_empty(struct cpc_host_device *cpc_hd)
{
	bool empty;

	mutex_lock(&cpc_hd->lock);
	empty = skb_queue_empty(&cpc_hd->tx_queue);
	mutex_unlock(&cpc_hd->lock);

	return empty;
}
EXPORT_SYMBOL_GPL(cpc_hd_tx_queue_empty);

/**
 * cpc_hd_dequeue() - Get the next SKB that was queued for transmission.
 * @cpc_hd: CPC Host Device.
 *
 * Get an SKB that was previously queued by cpc_hd_send_skb().
 *
 * Return: An SKB, or %NULL if queue was empty.
 */
struct sk_buff *cpc_hd_dequeue(struct cpc_host_device *cpc_hd)
{
	struct sk_buff *skb;

	mutex_lock(&cpc_hd->lock);
	skb = skb_dequeue(&cpc_hd->tx_queue);
	mutex_unlock(&cpc_hd->lock);

	return skb;
}
EXPORT_SYMBOL_GPL(cpc_hd_dequeue);

/**
 * cpc_hd_dequeue_many() - Get the next max_frames SKBs that were queued for transmission.
 * @cpc_hd: CPC host device.
 * @frame_list: Caller-provided sk_buff_head to fill with dequeued frames.
 * @max_frames: Maximum number of frames to dequeue.
 *
 * Return: Number of frames actually dequeued.
 */
u32 cpc_hd_dequeue_many(struct cpc_host_device *cpc_hd, struct sk_buff_head *frame_list,
			unsigned int max_frames)
{
	struct sk_buff *skb;
	unsigned int count = 0;

	mutex_lock(&cpc_hd->lock);
	while (count < max_frames && (skb = skb_dequeue(&cpc_hd->tx_queue))) {
		skb_queue_tail(frame_list, skb);
		count++;
	}
	mutex_unlock(&cpc_hd->lock);

	return count;
}
EXPORT_SYMBOL_GPL(cpc_hd_dequeue_many);

MODULE_DESCRIPTION("Greybus over CPC");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Silicon Laboratories, Inc.");
