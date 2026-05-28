/* SPDX-License-Identifier: GPL-2.0 or MIT */
/* Copyright 2026 ARM Limited. All rights reserved. */

#ifndef __PANTHOR_AM_H__
#define __PANTHOR_AM_H__

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/kfifo.h>
#include <linux/types.h>

#include "panthor_device_io.h"

/* Protocol versioning */
#define AM_MSG_MIN_SUPPORTED_VERSION 0x1
#define AM_MSG_CURRENT_VERSION 0x1

#define AM_INCOMING_MESSAGE0			0x0
#define AM_INCOMING_MESSAGE1			0x4
#define AM_OUTGOING_MESSAGE_STATUS		0x8
#define AM_OUTGOING_MESSAGE0			0xc
#define AM_OUTGOING_MESSAGE1			0x10

/* Arbiter to VM message IDs */
#define ARB_VM_GPU_STOP 0x01
#define ARB_VM_INIT 0x04

/* VM to Arbiter message IDs */
#define VM_ARB_INIT 0x05
#define VM_ARB_GPU_REQUEST 0x08
#define VM_ARB_GPU_STOPPED 0x09

#define AM_MSG_ID_MASK				GENMASK(7, 0)
#define AM_MSG_ACK_MASK				BIT(8)
#define AM_MSG_VERSION_MASK			GENMASK(15, 9)

#define AM_MSG_ID_GET(x)			FIELD_GET(AM_MSG_ID_MASK, x)
#define AM_MSG_ACK_GET(x)			FIELD_GET(AM_MSG_ACK_MASK, x)
#define AM_MSG_VERSION_GET(x)			FIELD_GET(AM_MSG_VERSION_MASK, x)

#define AM_MSG_INIT_MAKE(_id, _ack, _version) \
	((_id) | FIELD_PREP(AM_MSG_ACK_MASK, _ack) | FIELD_PREP(AM_MSG_VERSION_MASK, _version))

#define ARB_VM_INIT_MAKE(_ack, _version)	AM_MSG_INIT_MAKE(ARB_VM_INIT, _ack, _version)
#define VM_ARB_INIT_MAKE(_ack, _version)	AM_MSG_INIT_MAKE(VM_ARB_INIT, _ack, _version)

#define AM_MSG_DEFAULT_FIFO_SIZE		4

struct panthor_am_msg {
	/** @iomem: CPU mapping of the AM_MESSAGE registers */
	void __iomem *iomem;

	/** @lock: Lock for send_fifo and outgoing message register */
	spinlock_t lock;

	/** @send_fifo: FIFO to queue of messages to send */
	DECLARE_KFIFO(send_fifo, u64, AM_MSG_DEFAULT_FIFO_SIZE);

	/** @version: Protocol version */
	u8 version;
};

static inline void panthor_am_msg_init(struct panthor_am_msg *msg,
				       void __iomem *iomem)
{
	spin_lock_init(&msg->lock);
	INIT_KFIFO(msg->send_fifo);
	msg->iomem = iomem;
}

static inline bool panthor_am_msg_pending(struct panthor_am_msg *msg)
{
	return !!gpu_read(msg->iomem, AM_OUTGOING_MESSAGE_STATUS);
}

static inline u64 panthor_am_msg_read(struct panthor_am_msg *msg)
{
	return gpu_read64(msg->iomem, AM_INCOMING_MESSAGE0);
}

static inline void panthor_am_msg_write(struct panthor_am_msg *msg, u64 message)
{
	lockdep_assert_held(&msg->lock);

	/*
	 * Registers must be written in this exact order to prevent interrupts
	 * being raised before the complete message is written.
	 */
	gpu_write(msg->iomem, AM_OUTGOING_MESSAGE0, lower_32_bits(message));
	gpu_write(msg->iomem, AM_OUTGOING_MESSAGE1, upper_32_bits(message));
}

static inline int panthor_am_msg_retry(struct panthor_am_msg *msg)
{
	u64 message;

	guard(spinlock_irqsave)(&msg->lock);

	if (kfifo_is_empty(&msg->send_fifo))
		return 0;

	if (panthor_am_msg_pending(msg))
		return -EBUSY;

	/* FIFO should never be empty at this point */
	if (!kfifo_get(&msg->send_fifo, &message))
		return -EINVAL;

	panthor_am_msg_write(msg, message);

	/* There are still messages in the FIFO, notify caller to retry again */
	if (!kfifo_is_empty(&msg->send_fifo))
		return -EAGAIN;

	return 0;
}

static inline int panthor_am_msg_send(struct panthor_am_msg *msg, u64 message)
{
	guard(spinlock_irqsave)(&msg->lock);

	/*
	 * If there already is a pending message in the FIFO or the outgoing
	 * message is still not read by the receipient, add to the FIFO.
	 */
	if (!kfifo_is_empty(&msg->send_fifo) || panthor_am_msg_pending(msg)) {
		if (!kfifo_put(&msg->send_fifo, message))
			return -ENOMEM;

		/*
		 * return -EBUSY to indicate to the caller to schedule work to
		 * retry sending messages in the FIFO.
		 */
		return -EBUSY;
	}

	/* We are free to write to AM_OUTGOING_MESSAGE */
	panthor_am_msg_write(msg, message);

	return 0;
}

static inline int panthor_am_msg_version_validate(struct panthor_am_msg *msg,
						  u8 version)
{
	if (version < AM_MSG_MIN_SUPPORTED_VERSION) {
		msg->version = AM_MSG_MIN_SUPPORTED_VERSION;
		return -EOPNOTSUPP;
	}

	msg->version = min(version, AM_MSG_CURRENT_VERSION);

	return 0;
}

#endif
