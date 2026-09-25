// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2014      Protonic Holland,
 *                         David Jander
 * Copyright (C) 2014-2021, 2023 Pengutronix,
 *                         Marc Kleine-Budde <kernel@pengutronix.de>
 */

#include <linux/can/dev.h>
#include <linux/can/rx-offload.h>

struct can_rx_offload_cb {
	u32 timestamp;
};

static inline struct can_rx_offload_cb *
can_rx_offload_get_cb(struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct can_rx_offload_cb) > sizeof(skb->cb));

	return (struct can_rx_offload_cb *)skb->cb;
}

static inline bool
can_rx_offload_le(struct can_rx_offload_irq *offload_irq,
		  unsigned int a, unsigned int b)
{
	if (offload_irq->inc)
		return a <= b;
	else
		return a >= b;
}

static inline unsigned int
can_rx_offload_inc(struct can_rx_offload_irq *offload_irq, unsigned int *val)
{
	if (offload_irq->inc)
		return (*val)++;
	else
		return (*val)--;
}

static int can_rx_offload_napi_poll(struct napi_struct *napi, int quota)
{
	struct can_rx_offload *offload = container_of(napi,
						      struct can_rx_offload,
						      napi);
	struct net_device *dev = offload->dev;
	struct net_device_stats *stats = &dev->stats;
	struct sk_buff *skb;
	int work_done = 0;

	while ((work_done < quota) &&
	       (skb = skb_dequeue(&offload->skb_queue))) {
		struct can_frame *cf = (struct can_frame *)skb->data;

		work_done++;
		if (!(cf->can_id & CAN_ERR_FLAG)) {
			stats->rx_packets++;
			if (!(cf->can_id & CAN_RTR_FLAG))
				stats->rx_bytes += cf->len;
		}
		netif_receive_skb(skb);
	}

	if (work_done < quota) {
		napi_complete_done(napi, work_done);

		/* Check if there was another interrupt */
		if (!skb_queue_empty(&offload->skb_queue))
			napi_schedule(&offload->napi);
	}

	return work_done;
}

static inline void
__skb_queue_add_sort(struct sk_buff_head *head, struct sk_buff *new,
		     int (*compare)(struct sk_buff *a, struct sk_buff *b))
{
	struct sk_buff *pos, *insert = NULL;

	skb_queue_reverse_walk(head, pos) {
		const struct can_rx_offload_cb *cb_pos, *cb_new;

		cb_pos = can_rx_offload_get_cb(pos);
		cb_new = can_rx_offload_get_cb(new);

		netdev_dbg(new->dev,
			   "%s: pos=0x%08x, new=0x%08x, diff=%10d, queue_len=%d\n",
			   __func__,
			   cb_pos->timestamp, cb_new->timestamp,
			   cb_new->timestamp - cb_pos->timestamp,
			   skb_queue_len(head));

		if (compare(pos, new) < 0)
			continue;
		insert = pos;
		break;
	}
	if (!insert)
		__skb_queue_head(head, new);
	else
		__skb_queue_after(head, insert, new);
}

static int can_rx_offload_compare(struct sk_buff *a, struct sk_buff *b)
{
	const struct can_rx_offload_cb *cb_a, *cb_b;

	cb_a = can_rx_offload_get_cb(a);
	cb_b = can_rx_offload_get_cb(b);

	/* Subtract two u32 and return result as int, to keep
	 * difference steady around the u32 overflow.
	 */
	return cb_b->timestamp - cb_a->timestamp;
}

/**
 * can_rx_offload_offload_one() - Read one CAN frame from HW
 * @offload_irq: pointer to rx_offload per-IRQ context
 * @n: number of mailbox to read
 *
 * The task of this function is to read a CAN frame from mailbox @n
 * from the device and return the mailbox's content as a struct
 * sk_buff.
 *
 * If the struct can_rx_offload::skb_queue exceeds the maximal queue
 * length (struct can_rx_offload_irq::skb_queue_len_max) or no skb can
 * be allocated, the mailbox contents is discarded by reading it into
 * an overflow buffer. This way the mailbox is marked as free by the
 * driver.
 *
 * Return: A pointer to skb containing the CAN frame on success.
 *
 *         NULL if the mailbox @n is empty.
 *
 *         ERR_PTR() in case of an error
 */
static struct sk_buff *
can_rx_offload_offload_one(struct can_rx_offload_irq *offload_irq,
			   unsigned int n)
{
	struct can_rx_offload *offload = offload_irq->offload;
	struct sk_buff *skb;
	struct can_rx_offload_cb *cb;
	bool drop = false;
	u32 timestamp;

	/* If queue is full drop frame */
	if (unlikely(skb_queue_len(&offload->skb_queue) >
		     offload_irq->skb_queue_len_max))
		drop = true;

	skb = offload->mailbox_read(offload, n, &timestamp, drop);
	/* Mailbox was empty. */
	if (unlikely(!skb))
		return NULL;

	/* There was a problem reading the mailbox, propagate
	 * error value.
	 */
	if (IS_ERR(skb)) {
		offload->dev->stats.rx_dropped++;
		offload->dev->stats.rx_fifo_errors++;

		return skb;
	}

	/* Mailbox was read. */
	cb = can_rx_offload_get_cb(skb);
	cb->timestamp = timestamp;

	return skb;
}

int can_rx_offload_irq_offload_timestamp(struct can_rx_offload_irq *offload_irq,
					 u64 pending)
{
	unsigned int i;
	int received = 0;

	for (i = offload_irq->mb_first;
	     can_rx_offload_le(offload_irq, i, offload_irq->mb_last);
	     can_rx_offload_inc(offload_irq, &i)) {
		struct sk_buff *skb;

		if (!(pending & BIT_ULL(i)))
			continue;

		skb = can_rx_offload_offload_one(offload_irq, i);
		if (IS_ERR_OR_NULL(skb))
			continue;

		__skb_queue_add_sort(&offload_irq->skb_irq_queue, skb,
				     can_rx_offload_compare);
		received++;
	}

	return received;
}
EXPORT_SYMBOL_GPL(can_rx_offload_irq_offload_timestamp);

int can_rx_offload_irq_offload_fifo(struct can_rx_offload_irq *offload_irq)
{
	struct sk_buff *skb;
	int received = 0;

	while (1) {
		skb = can_rx_offload_offload_one(offload_irq, 0);
		if (IS_ERR(skb))
			continue;
		if (!skb)
			break;

		__skb_queue_tail(&offload_irq->skb_irq_queue, skb);
		received++;
	}

	return received;
}
EXPORT_SYMBOL_GPL(can_rx_offload_irq_offload_fifo);

int can_rx_offload_queue_timestamp(struct can_rx_offload_irq *offload_irq,
				   struct sk_buff *skb, u32 timestamp)
{
	struct can_rx_offload *offload = offload_irq->offload;
	struct can_rx_offload_cb *cb;

	if (skb_queue_len(&offload->skb_queue) >
	    offload_irq->skb_queue_len_max) {
		dev_kfree_skb_any(skb);
		return -ENOBUFS;
	}

	cb = can_rx_offload_get_cb(skb);
	cb->timestamp = timestamp;

	__skb_queue_add_sort(&offload_irq->skb_irq_queue, skb,
			     can_rx_offload_compare);

	return 0;
}
EXPORT_SYMBOL_GPL(can_rx_offload_queue_timestamp);

unsigned int
can_rx_offload_get_echo_skb_queue_timestamp(struct can_rx_offload_irq *offload_irq,
					    unsigned int idx, u32 timestamp,
					    unsigned int *frame_len_ptr)
{
	struct net_device *dev = offload_irq->offload->dev;
	struct net_device_stats *stats = &dev->stats;
	struct sk_buff *skb;
	unsigned int len;
	int err;

	skb = __can_get_echo_skb(dev, idx, &len, frame_len_ptr);
	if (!skb)
		return 0;

	err = can_rx_offload_queue_timestamp(offload_irq, skb, timestamp);
	if (err) {
		stats->rx_errors++;
		stats->tx_fifo_errors++;
	}

	return len;
}
EXPORT_SYMBOL_GPL(can_rx_offload_get_echo_skb_queue_timestamp);

int can_rx_offload_queue_tail(struct can_rx_offload_irq *offload_irq,
			      struct sk_buff *skb)
{
	struct can_rx_offload *offload = offload_irq->offload;

	if (skb_queue_len(&offload->skb_queue) >
	    offload_irq->skb_queue_len_max) {
		dev_kfree_skb_any(skb);
		return -ENOBUFS;
	}

	__skb_queue_tail(&offload_irq->skb_irq_queue, skb);

	return 0;
}
EXPORT_SYMBOL_GPL(can_rx_offload_queue_tail);

unsigned int
can_rx_offload_get_echo_skb_queue_tail(struct can_rx_offload_irq *offload_irq,
				       unsigned int idx,
				       unsigned int *frame_len_ptr)
{
	struct net_device *dev = offload_irq->offload->dev;
	struct net_device_stats *stats = &dev->stats;
	struct sk_buff *skb;
	unsigned int len;
	int err;

	skb = __can_get_echo_skb(dev, idx, &len, frame_len_ptr);
	if (!skb)
		return 0;

	err = can_rx_offload_queue_tail(offload_irq, skb);
	if (err) {
		stats->rx_errors++;
		stats->tx_fifo_errors++;
	}

	return len;
}
EXPORT_SYMBOL_GPL(can_rx_offload_get_echo_skb_queue_tail);

static void
can_rx_offload_move_to_skb_queue(struct can_rx_offload_irq *offload_irq)
{
	struct can_rx_offload *offload = offload_irq->offload;
	unsigned long flags;

	spin_lock_irqsave(&offload->skb_queue.lock, flags);

	if (offload->irq_cnt > 1 && offload->sort) {
		/* Merge this IRQ's sorted queue into skb_queue to keep the
		 * overall timestamp order.
		 */
		struct sk_buff *skb;

		while ((skb = __skb_dequeue(&offload_irq->skb_irq_queue)))
			__skb_queue_add_sort(&offload->skb_queue, skb,
					     can_rx_offload_compare);
	} else {
		/* Single producer, or no timestamp to sort by. */
		skb_queue_splice_tail_init(&offload_irq->skb_irq_queue,
					   &offload->skb_queue);
	}

	spin_unlock_irqrestore(&offload->skb_queue.lock, flags);
}

void can_rx_offload_irq_finish(struct can_rx_offload_irq *offload_irq)
{
	struct can_rx_offload *offload = offload_irq->offload;
	int queue_len;

	if (skb_queue_empty_lockless(&offload_irq->skb_irq_queue))
		return;

	can_rx_offload_move_to_skb_queue(offload_irq);

	queue_len = skb_queue_len(&offload->skb_queue);
	if (queue_len > offload_irq->skb_queue_len_max / 8)
		netdev_dbg(offload->dev, "%s: queue_len=%d\n",
			   __func__, queue_len);

	napi_schedule(&offload->napi);
}
EXPORT_SYMBOL_GPL(can_rx_offload_irq_finish);

void can_rx_offload_threaded_irq_finish(struct can_rx_offload_irq *offload_irq)
{
	struct can_rx_offload *offload = offload_irq->offload;
	int queue_len;

	if (skb_queue_empty_lockless(&offload_irq->skb_irq_queue))
		return;

	can_rx_offload_move_to_skb_queue(offload_irq);

	queue_len = skb_queue_len(&offload->skb_queue);
	if (queue_len > offload_irq->skb_queue_len_max / 8)
		netdev_dbg(offload->dev, "%s: queue_len=%d\n",
			   __func__, queue_len);

	local_bh_disable();
	napi_schedule(&offload->napi);
	local_bh_enable();
}
EXPORT_SYMBOL_GPL(can_rx_offload_threaded_irq_finish);

static int can_rx_offload_init_queue(struct net_device *dev,
				     struct can_rx_offload *offload,
				     struct can_rx_offload_irq *offload_irq,
				     unsigned int weight)
{
	struct can_rx_offload_irq *pos;
	u32 skb_queue_len_max;

	offload_irq->offload = offload;
	__skb_queue_head_init(&offload_irq->skb_irq_queue);

	/* The first registered IRQ initializes the shared state. */
	if (!offload->irq_cnt) {
		offload->dev = dev;
		skb_queue_head_init(&offload->skb_queue);
		INIT_LIST_HEAD(&offload->irqs);
		netif_napi_add_weight(dev, &offload->napi,
				      can_rx_offload_napi_poll, weight);
	} else if (weight > offload->napi.weight) {
		/* All contexts feed the same NAPI, keep the largest weight. */
		offload->napi.weight = weight;
	}
	list_add_tail(&offload_irq->node, &offload->irqs);
	offload->irq_cnt++;

	/* Limit queue len to 4x the weight (rounded to next power of two).
	 * All contexts feed the same skb_queue, so they share its limit.
	 */
	skb_queue_len_max = 2 << fls(offload->napi.weight);
	skb_queue_len_max *= 4;
	list_for_each_entry(pos, &offload->irqs, node)
		pos->skb_queue_len_max = skb_queue_len_max;

	dev_dbg(dev->dev.parent, "%s: skb_queue_len_max=%d\n",
		__func__, skb_queue_len_max);

	return 0;
}

int can_rx_offload_add_timestamp(struct net_device *dev,
				 struct can_rx_offload *offload,
				 struct can_rx_offload_irq *offload_irq)
{
	unsigned int weight;

	if (offload_irq->mb_first > BITS_PER_LONG_LONG ||
	    offload_irq->mb_last > BITS_PER_LONG_LONG || !offload->mailbox_read)
		return -EINVAL;

	if (offload_irq->mb_first < offload_irq->mb_last) {
		offload_irq->inc = true;
		weight = offload_irq->mb_last - offload_irq->mb_first;
	} else {
		offload_irq->inc = false;
		weight = offload_irq->mb_first - offload_irq->mb_last;
	}

	offload->sort = true;

	return can_rx_offload_init_queue(dev, offload, offload_irq, weight);
}
EXPORT_SYMBOL_GPL(can_rx_offload_add_timestamp);

int can_rx_offload_add_fifo(struct net_device *dev,
			    struct can_rx_offload *offload,
			    struct can_rx_offload_irq *offload_irq,
			    unsigned int weight)
{
	if (!offload->mailbox_read)
		return -EINVAL;

	return can_rx_offload_init_queue(dev, offload, offload_irq, weight);
}
EXPORT_SYMBOL_GPL(can_rx_offload_add_fifo);

int can_rx_offload_add_manual(struct net_device *dev,
			      struct can_rx_offload *offload,
			      struct can_rx_offload_irq *offload_irq,
			      unsigned int weight)
{
	if (offload->mailbox_read)
		return -EINVAL;

	return can_rx_offload_init_queue(dev, offload, offload_irq, weight);
}
EXPORT_SYMBOL_GPL(can_rx_offload_add_manual);

void can_rx_offload_enable(struct can_rx_offload *offload)
{
	napi_enable(&offload->napi);
}
EXPORT_SYMBOL_GPL(can_rx_offload_enable);

void can_rx_offload_del(struct can_rx_offload *offload)
{
	struct can_rx_offload_irq *offload_irq, *tmp;

	netif_napi_del(&offload->napi);
	skb_queue_purge(&offload->skb_queue);

	/* Drop the contexts, so that re-adding them (e.g. on the next
	 * ndo_open()) initializes the shared state again.
	 */
	list_for_each_entry_safe(offload_irq, tmp, &offload->irqs, node) {
		__skb_queue_purge(&offload_irq->skb_irq_queue);
		list_del_init(&offload_irq->node);
	}
	offload->irq_cnt = 0;
	offload->sort = false;
}
EXPORT_SYMBOL_GPL(can_rx_offload_del);
