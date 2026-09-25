/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/can/rx-offload.h
 *
 * Copyright (c) 2014 David Jander, Protonic Holland
 * Copyright (c) 2014-2017, 2023 Pengutronix, Marc Kleine-Budde <kernel@pengutronix.de>
 */

#ifndef _CAN_RX_OFFLOAD_H
#define _CAN_RX_OFFLOAD_H

#include <linux/netdevice.h>
#include <linux/can.h>

struct can_rx_offload {
	struct net_device *dev;

	struct sk_buff *(*mailbox_read)(struct can_rx_offload *offload,
					unsigned int mb, u32 *timestamp,
					bool drop);

	struct sk_buff_head skb_queue;

	/* Registered per-IRQ contexts, only changed on add/del. A single
	 * context lets the finish path splice, more than one makes it sort
	 * the skbs into skb_queue.
	 */
	struct list_head irqs;
	unsigned int irq_cnt;

	/* Set if the queued skbs carry a timestamp to sort them by. */
	bool sort;

	/* weight is the largest one over the registered per-IRQ contexts */
	struct napi_struct napi;
};

/* Per-IRQ receive context, one per IRQ line producing RX frames.
 * skb_irq_queue has a single producer, so it needs no lock. The finish
 * helpers move the skbs into can_rx_offload::skb_queue under its lock.
 */
struct can_rx_offload_irq {
	struct can_rx_offload *offload;

	struct sk_buff_head skb_irq_queue;
	u32 skb_queue_len_max;

	unsigned int mb_first;
	unsigned int mb_last;

	bool inc;

	/* entry on can_rx_offload::irqs */
	struct list_head node;
};

int can_rx_offload_add_timestamp(struct net_device *dev,
				 struct can_rx_offload *offload,
				 struct can_rx_offload_irq *offload_irq);
int can_rx_offload_add_fifo(struct net_device *dev,
			    struct can_rx_offload *offload,
			    struct can_rx_offload_irq *offload_irq,
			    unsigned int weight);
int can_rx_offload_add_manual(struct net_device *dev,
			      struct can_rx_offload *offload,
			      struct can_rx_offload_irq *offload_irq,
			      unsigned int weight);
int can_rx_offload_irq_offload_timestamp(struct can_rx_offload_irq *offload_irq,
					 u64 reg);
int can_rx_offload_irq_offload_fifo(struct can_rx_offload_irq *offload_irq);
int can_rx_offload_queue_timestamp(struct can_rx_offload_irq *offload_irq,
				   struct sk_buff *skb, u32 timestamp);
unsigned int can_rx_offload_get_echo_skb_queue_timestamp(struct can_rx_offload_irq *offload_irq,
							 unsigned int idx, u32 timestamp,
							 unsigned int *frame_len_ptr);
int can_rx_offload_queue_tail(struct can_rx_offload_irq *offload_irq,
			      struct sk_buff *skb);
unsigned int can_rx_offload_get_echo_skb_queue_tail(struct can_rx_offload_irq *offload_irq,
						    unsigned int idx,
						    unsigned int *frame_len_ptr);
void can_rx_offload_irq_finish(struct can_rx_offload_irq *offload_irq);
void can_rx_offload_threaded_irq_finish(struct can_rx_offload_irq *offload_irq);
void can_rx_offload_del(struct can_rx_offload *offload);
void can_rx_offload_enable(struct can_rx_offload *offload);

static inline void can_rx_offload_disable(struct can_rx_offload *offload)
{
	napi_disable(&offload->napi);
}

#endif /* !_CAN_RX_OFFLOAD_H */
