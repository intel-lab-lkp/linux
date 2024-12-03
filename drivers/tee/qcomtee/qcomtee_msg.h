/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef QCOMTEE_MSG_H
#define QCOMTEE_MSG_H

#include <linux/firmware/qcom/qcom_tee.h>

/**
 * DOC: ''Qualcomm TEE'' (QTEE) Transport Message
 *
 * There are two buffers shared with QTEE, inbound and outbound buffers.
 * The inbound buffer is used for direct object invocation and the outbound buffer is
 * used to make a request from QTEE to kernel, i.e. callback request.
 *
 * The unused tail of the outbound buffer is also used for sending and receiving
 * asynchronous messages. An asynchronous message is independent from the current
 * object invocation (i.e. contents of the inbound buffer) or callback request
 * (i.e. the head of the outbound buffer), see qcom_tee_get_async_buffer(). It is
 * used by endpoints (QTEE or kernel) as an optimization to reduce number of context
 * switches between secure and non-secure world.
 *
 * For instance, QTEE never sends an explicit callback request to release an object in
 * kernel. Instead, it sends asynchronous release messages in outbound buffer when QTEE
 * returns from previous direct object invocation, or append asynchronous release
 * messages after the current callback request.
 *
 * QTEE supports two types of arguments in a message: buffer and object arguments.
 * Depending on the direction of data flow, they could be input buffer (IO) to QTEE,
 * output buffer (OB) from QTEE, input object (IO) to QTEE, or output object (OO) from
 * QTEE. Object arguments hold object ids. Buffer arguments hold (offset, size) pairs
 * into the inbound or outbound buffers.
 *
 * QTEE holds an object table for objects, it hosts and exposes to kernel. An object id
 * is an index to the object table in QTEE.
 *
 * For direct object invocation message format in inbound buffer see
 * &struct qcom_tee_msg_object_invoke. For callback request message format in outbound
 * buffer see &struct qcom_tee_msg_callback. For the message format for asynchronous message
 * in outbound buffer see &struct qcom_tee_async_msg_hdr.
 */

/**
 * define QCOM_TEE_MSG_OBJECT_NS_BIT - Non-secure bit
 *
 * Object id is a globally unique 32-bit number. Ids referencing objects in kernel should
 * have %QCOM_TEE_MSG_OBJECT_NS_BIT set.
 */
#define QCOM_TEE_MSG_OBJECT_NS_BIT BIT(31)

/* Static object ids recognized by QTEE. */
#define QCOM_TEE_MSG_OBJECT_NULL (0U)
#define QCOM_TEE_MSG_OBJECT_ROOT (1U)

/* Definitions from QTEE as part of the transport protocol. */

/* qcom_tee_msg_arg is argument as recognized by QTEE. */
union qcom_tee_msg_arg {
	struct {
		u32 offset;
		u32 size;
	} b;
	u32 o;
};

/* BI and BO payloads in a QTEE messages should be at 64-bit boundaries. */
#define qcom_tee_msg_offset_align(o) ALIGN((o), sizeof(u64))

/* Operation for objects is 32-bit. Transport uses upper 16-bits internally. */
#define QCOM_TEE_MSG_OBJECT_OP_MASK 0x0000FFFFU

/* Reserved Operation IDs sent to QTEE: */
/* QCOM_TEE_MSG_OBJECT_OP_RELEASE - Reduces the refcount and releases the object.
 * QCOM_TEE_MSG_OBJECT_OP_RETAIN  - Increases the refcount.
 *
 * These operation id are valid for all objects. They are not available outside of this
 * driver. Developers should use qcom_tee_object_get() and qcom_tee_object_put(), to
 * achieve the same.
 */

#define QCOM_TEE_MSG_OBJECT_OP_RELEASE	(QCOM_TEE_MSG_OBJECT_OP_MASK - 0)
#define QCOM_TEE_MSG_OBJECT_OP_RETAIN	(QCOM_TEE_MSG_OBJECT_OP_MASK - 1)

/**
 * struct qcom_tee_msg_object_invoke - Direct object invocation message
 * @ctx: object id hosted in QTEE
 * @op: operation for the object
 * @counts: number of different type of arguments in @args
 * @args: array of arguments
 *
 * @counts consists of 4 * 4-bits felids. Bits 0 - 3, is number of input buffers,
 * bits 4 - 7, is number of output buffers, bits 8 - 11, is number of input objects,
 * and bits 12 - 15, is number of output objects. Remaining bits should be zero.
 *
 * Maximum number of arguments of each type is defined by %QCOM_TEE_ARGS_PER_TYPE.
 */
struct qcom_tee_msg_object_invoke {
	u32 cxt;
	u32 op;
	u32 counts;
	union qcom_tee_msg_arg args[];
};

/**
 * struct qcom_tee_msg_callback - Callback request message
 * @result: result of operation @op on object referenced by @cxt
 * @cxt: object id hosted in kernel
 * @op: operation for the object
 * @counts: number of different type of arguments in @args
 * @args: array of arguments
 *
 * For details of @counts, see &qcom_tee_msg_object_invoke.counts.
 */
struct qcom_tee_msg_callback {
	u32 result;
	u32 cxt;
	u32 op;
	u32 counts;
	union qcom_tee_msg_arg args[];
};

/* Offset in the message for the beginning of buffer argument's contents. */
#define qcom_tee_msg_buffer_args(t, n) \
	qcom_tee_msg_offset_align(struct_size_t(t, args, n))
/* Pointer to the beginning of a buffer argument's content at an offset in a message. */
#define qcom_tee_msg_offset_to_ptr(m, off) ((void *)&((char *)(m))[(off)])

/* Some helpers to manage msg.counts. */

#define QCOM_TEE_MSG_NUM_IB(x) ((x) & 0xfU)
#define QCOM_TEE_MSG_NUM_OB(x) (((x) >> 4) & 0xfU)
#define QCOM_TEE_MSG_NUM_IO(x) (((x) >> 8) & 0xfU)
#define QCOM_TEE_MSG_NUM_OO(x) (((x) >> 12) & 0xfU)

#define QCOM_TEE_MSG_IDX_IB(x) (0U)
#define QCOM_TEE_MSG_IDX_OB(x) (QCOM_TEE_MSG_IDX_IB(x) + QCOM_TEE_MSG_NUM_IB(x))
#define QCOM_TEE_MSG_IDX_IO(x) (QCOM_TEE_MSG_IDX_OB(x) + QCOM_TEE_MSG_NUM_OB(x))
#define QCOM_TEE_MSG_IDX_OO(x) (QCOM_TEE_MSG_IDX_IO(x) + QCOM_TEE_MSG_NUM_IO(x))

#define qcom_tee_msg_for_each(i, c, type)	\
	for (i = QCOM_TEE_MSG_IDX_##type(c);	\
	     i < (QCOM_TEE_MSG_IDX_##type(c) + QCOM_TEE_MSG_NUM_##type(c)); \
	     i++)

#define qcom_tee_msg_for_each_input_buffer(i, m)  qcom_tee_msg_for_each(i, (m)->counts, IB)
#define qcom_tee_msg_for_each_output_buffer(i, m) qcom_tee_msg_for_each(i, (m)->counts, OB)
#define qcom_tee_msg_for_each_input_object(i, m)  qcom_tee_msg_for_each(i, (m)->counts, IO)
#define qcom_tee_msg_for_each_output_object(i, m) qcom_tee_msg_for_each(i, (m)->counts, OO)

/* Sum of arguments in a message. */
#define qcom_tee_msg_args(m) (QCOM_TEE_MSG_IDX_OO((m)->counts) + QCOM_TEE_MSG_NUM_OO((m)->counts))

static inline void qcom_tee_msg_init(struct qcom_tee_msg_object_invoke *msg, u32 cxt, u32 op,
				     int in_buffer, int out_buffer, int in_object, int out_object)
{
	msg->counts |= (in_buffer & 0xfU);
	msg->counts |= ((out_buffer - in_buffer) & 0xfU) << 4;
	msg->counts |= ((in_object - out_buffer) & 0xfU) << 8;
	msg->counts |= ((out_object - in_object) & 0xfU) << 12;
	msg->cxt = cxt;
	msg->op = op;
}

/* Generic error codes. */
#define QCOM_TEE_MSG_OK			0	/* non-specific success code. */
#define QCOM_TEE_MSG_ERROR		1	/* non-specific error. */
#define QCOM_TEE_MSG_ERROR_INVALID	2	/* unsupported/unrecognized request. */
#define QCOM_TEE_MSG_ERROR_SIZE_IN	3	/* supplied buffer/string too large. */
#define QCOM_TEE_MSG_ERROR_SIZE_OUT	4	/* supplied output buffer too small. */
#define QCOM_TEE_MSG_ERROR_USERBASE	10	/* start of user-defined error range. */

/* Transport layer error codes. */
#define QCOM_TEE_MSG_ERROR_DEFUNCT	-90	/* object no longer exists. */
#define QCOM_TEE_MSG_ERROR_ABORT	-91	/* calling thread must exit. */
#define QCOM_TEE_MSG_ERROR_BADOBJ	-92	/* invalid object context. */
#define QCOM_TEE_MSG_ERROR_NOSLOTS	-93	/* caller's object table full. */
#define QCOM_TEE_MSG_ERROR_MAXARGS	-94	/* too many args. */
#define QCOM_TEE_MSG_ERROR_MAXDATA	-95	/* buffers too large. */
#define QCOM_TEE_MSG_ERROR_UNAVAIL	-96	/* the request could not be processed. */
#define QCOM_TEE_MSG_ERROR_KMEM		-97	/* kernel out of memory. */
#define QCOM_TEE_MSG_ERROR_REMOTE	-98	/* local method sent to remote object. */
#define QCOM_TEE_MSG_ERROR_BUSY		-99	/* Object is busy. */
#define QCOM_TEE_MSG_ERROR_TIMEOUT	-103	/* Call Back Object invocation timed out. */

static inline void qcom_tee_msg_translate_err(struct qcom_tee_msg_callback *cb_msg, int err)
{
	if (!err) {
		cb_msg->result = QCOM_TEE_MSG_OK;
	} else if (err < 0) {
		/* If err < 0, then it is a transport error. */
		switch (err) {
		case -ENOMEM:
			cb_msg->result = QCOM_TEE_MSG_ERROR_KMEM;
			break;
		case -ENODEV:
			cb_msg->result = QCOM_TEE_MSG_ERROR_DEFUNCT;
			break;
		case -ENOSPC:
		case -EBUSY:
			cb_msg->result = QCOM_TEE_MSG_ERROR_BUSY;
			break;
		case -EBADF:
		case -EINVAL:
			cb_msg->result = QCOM_TEE_MSG_ERROR_UNAVAIL;
			break;
		default:
			cb_msg->result =  QCOM_TEE_MSG_ERROR;
		}
	} else {
		/* If err > 0, then it is user defined error, pass it as is. */
		cb_msg->result = err;
	}
}

#endif /* QCOMTEE_MSG_H */
