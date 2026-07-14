/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Google LLC
 */

#ifndef _GOOG_MBA_PRIV_H_
#define _GOOG_MBA_PRIV_H_

#include <linux/mailbox_controller.h>

struct goog_mbox_info {
	/** @mbox: Mailbox controller structure. */
	struct mbox_controller mbox;

	/** @np: Device tree node */
	struct device_node *np;

	/** @chan: Mailbox channel structure; only 1 channel per mailbox. */
	struct mbox_chan chan;

	/** @mba: Pointer to the mailbox array containing this mailbox. */
	struct goog_mba_info *mba;

	/** @iomem: IO memory associated with this mailbox. */
	void __iomem *iomem;

	/** @irq: Linux IRQ associated with this mailbox. */
	int irq;

	/** @msg_buffer_words: Number of 32-bit words present in hardware. */
	unsigned int msg_buffer_words;

	/**
	 * @tx_payload_words: Number of 32-bit words in a tx mailbox message.
	 *
	 * In queue mode this is detected on the first TX transfer and subsequent
	 * transfers must match.
	 */
	unsigned int tx_payload_words;

	/** @rx_payload_words: Number of 32-bit words in a rx mailbox message. */
	unsigned int rx_payload_words;

	/**
	 * @rx_buffer: Memory storage for payload when receiving
	 *
	 * When we get a message from the other side we copy it here before
	 * acknowledging the message and passing it to the client. Buffer
	 * is `payload_words * 4` bytes big.
	 */
	u32 *rx_buffer;

	/**
	 * @queue_mode: If true, the other side uses the "queue mode" protocol.
	 *
	 * In the "queue mode" protocol, we can send more than one message at
	 * once and we treat the message buffer like a circular queue, with
	 * each entry being `payload_words` big.
	 */
	bool queue_mode;


	/* QUEUE MODE ONLY BELOW */

	/**
	 * @tx_idx: For queue mode, index into msg buffer to write the next msg.
	 *
	 * Always between 0 and msg_buffer_words - 1. Increments by payload_words
	 * after each transmission and wraps to 0 if it's == msg_buffer_words.
	 */
	unsigned int tx_idx;

	/**
	 * @rx_idx: For queue mode, index into msg buffer to read the next msg.
	 *
	 * Always between 0 and msg_buffer_words - 1. Increments by rx_payload_words
	 * after each reception and wraps to 0 if it's == msg_buffer_words.
	 */
	unsigned int rx_idx;

	/**
	 * @lock: For queue mode, protects outstanding_msgs
	 *
	 * We update `outstanding_msgs` in the interrupt handler and when
	 * queuing up a message. This protects those two accesses.
	 */
	spinlock_t lock;

	/**
	 * @outstanding_msgs: For queue mode, num msgs we've written but not acked
	 *
	 * After we start each transmission we grab the `lock` and increment
	 * this by 1. In the interrupt handler when we see that some messages
	 * were transferred we decrease this and send out the proper number
	 * of acks.
	 */
	unsigned int outstanding_msgs;
};

struct goog_mba_info {
	/** @dev: Pointer to the `struct device` */
	struct device *dev;

	/** @global_iomem: Pointer to global IO memory, or NULL */
	void __iomem *global_iomem;
};

#endif /* _GOOG_MBA_PRIV_H_ */
