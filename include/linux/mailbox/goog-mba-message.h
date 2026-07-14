/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Google MailBox Array (MBA) Mailbox Message
 *
 * Copyright (c) 2025 Google LLC
 */

#ifndef _LINUX_MAILBOX_GOOG_MBA_MESSAGE_H_
#define _LINUX_MAILBOX_GOOG_MBA_MESSAGE_H_

#include <linux/types.h>

struct goog_mba_tx_msg {
	/** @payload: The contents of the message to send. */
	const u32 *payload;

	/** @payload_words: The number of 32-bit words in the payload. */
	u8 payload_words;

	/**
	 * @init: Initialize queue settings after sending.
	 *
	 * Tell the mailbox driver that this is a special "initialize"
	 * message for a queue-based mailbox. This allows the mailbox driver
	 * to keep its state synced with the remote side of the mailbox.
	 */
	bool init;
};

struct goog_mba_rx_msg {
	/** @payload: The contents of the message received. */
	const u32 *payload;

	/** @payload_words: The number of 32-bit words in the payload. */
	u8 payload_words;
};

#endif /* _LINUX_MAILBOX_GOOG_MBA_MESSAGE_H_ */
