/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple DockChannel mailbox message format.
 *
 * Copyright The Asahi Linux Contributors
 */

#ifndef _LINUX_MAILBOX_APPLE_DOCKCHANNEL_H_
#define _LINUX_MAILBOX_APPLE_DOCKCHANNEL_H_

#include <linux/types.h>

/**
 * struct apple_dockchannel_msg - DockChannel mailbox payload
 * @data: Pointer to the byte stream payload
 * @len: Number of payload bytes
 *
 * For TX, @data must remain valid until mbox_send_message() completes or the
 * client receives tx_done in non-blocking mode.
 *
 * For RX, @data is owned by the controller and is valid only for the duration
 * of the rx_callback.
 */
struct apple_dockchannel_msg {
	void *data;
	size_t len;
};

#endif /* _LINUX_MAILBOX_APPLE_DOCKCHANNEL_H_ */
