// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/mutex.h>
#include <linux/slab.h>

#include "qcomtee_private.h"
#include "qcomtee_msg.h"

#define QCOM_TEE_ASYNC_VERSION_1_0 0x00010000U	/* Major: 0x0001, Minor: 0x0000. */
#define QCOM_TEE_ASYNC_VERSION_1_1 0x00010001U	/* Major: 0x0001, Minor: 0x0001. */
#define QCOM_TEE_ASYNC_VERSION_1_2 0x00010002U	/* Major: 0x0001, Minor: 0x0002. */
#define QCOM_TEE_ASYNC_VERSION QCOM_TEE_ASYNC_VERSION_1_2 /* Current Version. */

#define QCOM_TEE_ASYNC_VERSION_MAJOR(n) upper_16_bits(n)
#define QCOM_TEE_ASYNC_VERSION_MINOR(n) lower_16_bits(n)

/**
 * struct qcom_tee_async_msg_hdr - Asynchronous message header format.
 * @version: current async protocol version of remote endpoint
 * @op: async operation
 *
 * @version specifies the endpoints (QTEE or driver) supported async protocol, e.g.
 * if QTEE set @version to %QCOM_TEE_ASYNC_VERSION_1_1, QTEE handles operations
 * supported in %QCOM_TEE_ASYNC_VERSION_1_1 or %QCOM_TEE_ASYNC_VERSION_1_0.
 * @op determins the message format.
 */
struct qcom_tee_async_msg_hdr {
	u32 version;
	u32 op;
};

/**
 * struct qcom_tee_async_release_msg - Release asynchronous message.
 * @hdr: message header as &struct qcom_tee_async_msg_hdr
 * @counts: number of objects in @object_ids
 * @object_ids: array of object ids should be released
 *
 * Available in Major = 0x0001, Minor >= 0x0000.
 */
struct qcom_tee_async_release_msg {
	struct qcom_tee_async_msg_hdr hdr;
	u32 counts;
	u32 object_ids[] __counted_by(counts);
};

/**
 * qcom_tee_get_async_buffer() - Get start of the asynchronous message in outbound buffer.
 * @oic: context used for current invocation
 * @async_buffer: return buffer to extract from or fill in async messages
 *
 * If @oic is used for direct object invocation, whole outbound buffer is available for
 * async message. If @oic is used for callback request, the tail of outbound buffer (after
 * the callback request message) is available for async message.
 */
static void qcom_tee_get_async_buffer(struct qcom_tee_object_invoke_ctx *oic,
				      struct qcom_tee_buffer *async_buffer)
{
	struct qcom_tee_msg_callback *msg;
	unsigned int offset;
	int i;

	if (!(oic->flags & QCOM_TEE_OIC_FLAG_BUSY)) {
		/* The outbound buffer is empty. Using the whole buffer. */
		offset = 0;
	} else {
		msg = (struct qcom_tee_msg_callback *)oic->out_msg.addr;

		/* Start offset in a message for buffer arguments. */
		offset = qcom_tee_msg_buffer_args(struct qcom_tee_msg_callback,
						  qcom_tee_msg_args(msg));

		/* Add size of IB arguments. */
		qcom_tee_msg_for_each_input_buffer(i, msg)
			offset += qcom_tee_msg_offset_align(msg->args[i].b.size);

		/* Add size of OB arguments. */
		qcom_tee_msg_for_each_output_buffer(i, msg)
			offset += qcom_tee_msg_offset_align(msg->args[i].b.size);
	}

	async_buffer->addr = oic->out_msg.addr + offset;
	async_buffer->size = oic->out_msg.size - offset;
}

/**
 * qcom_tee_async_release_handler() - Process QTEE async requests for releasing objects.
 * @oic: context used for current invocation
 * @msg: async message for object release
 * @size: size of the async buffer available
 *
 * Return: Size of outbound buffer used when processing @msg.
 */
static size_t qcom_tee_async_release_handler(struct qcom_tee_object_invoke_ctx *oic,
					     struct qcom_tee_async_msg_hdr *async_msg, size_t size)
{
	struct qcom_tee_async_release_msg *msg = (struct qcom_tee_async_release_msg *)async_msg;
	struct qcom_tee_object *object;
	int i;

	for (i = 0; i < msg->counts; i++) {
		object = qcom_tee_idx_erase(msg->object_ids[i]);
		qcom_tee_object_put(object);
	}

	return struct_size_t(struct qcom_tee_async_release_msg, object_ids, i);
}

/**
 * qcom_tee_fetch_async_reqs() - Fetch and process asynchronous messages.
 * @oic: context used for current invocation
 *
 * It looks for handler to process the requested operations in the async message.
 * Currently, only support async release requests.
 */
void qcom_tee_fetch_async_reqs(struct qcom_tee_object_invoke_ctx *oic)
{
	struct qcom_tee_async_msg_hdr *async_msg;
	struct qcom_tee_buffer async_buffer;
	size_t consumed, used = 0;

	qcom_tee_get_async_buffer(oic, &async_buffer);

	while (async_buffer.size - used > sizeof(struct qcom_tee_async_msg_hdr)) {
		async_msg = (struct qcom_tee_async_msg_hdr *)(async_buffer.addr + used);

		if (QCOM_TEE_ASYNC_VERSION_MAJOR(async_msg->version) !=
		    QCOM_TEE_ASYNC_VERSION_MAJOR(QCOM_TEE_ASYNC_VERSION))
			goto out;

		switch (async_msg->op) {
		case QCOM_TEE_MSG_OBJECT_OP_RELEASE:
			consumed = qcom_tee_async_release_handler(oic, async_msg,
								  async_buffer.size - used);
			break;
		default:
			/* Unsupported operations. */
			goto out;
		}

		/* Supported operation but unable to parse the message. */
		if (!consumed)
			goto out;

		used += qcom_tee_msg_offset_align(consumed);
	}

 out:
	/* Reset the async messages buffer so async requests do not loopback to QTEE. */
	memzero_explicit(async_buffer.addr, async_buffer.size);
}
