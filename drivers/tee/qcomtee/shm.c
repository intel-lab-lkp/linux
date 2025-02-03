// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/mm.h>

#include "qcomtee_msg.h"
#include "qcomtee_private.h"

/**
 * define MAX_BUFFER_SIZE - Maximum size of inbound and outbound buffers.
 *
 * QTEE transport does not impose any restriction on these buffers.
 * However, if the size of the buffers is larger than %MAX_BUFFER_SIZE,
 * the user should probably use some other form of shared memory with QTEE.
 */
#define MAX_BUFFER_SIZE SZ_4M

/* Pool to allocate inbound and outbound buffers. */
static struct qcom_tzmem_pool *tzmem_msg_pool;

int qcomtee_msg_buffers_alloc(struct qcomtee_object_invoke_ctx *oic,
			      struct qcomtee_arg *u)
{
	size_t size;
	int i;

	/* Start offset in a message for buffer arguments. */
	size = qcomtee_msg_buffer_args(struct qcomtee_msg_object_invoke,
				       qcomtee_args_len(u));
	if (size > MAX_BUFFER_SIZE)
		return -EINVAL;

	/* Add size of IB arguments. */
	qcomtee_arg_for_each_input_buffer(i, u) {
		size = size_add(size, qcomtee_msg_offset_align(u[i].b.size));
		if (size > MAX_BUFFER_SIZE)
			return -EINVAL;
	}

	/* Add size of OB arguments. */
	qcomtee_arg_for_each_output_buffer(i, u) {
		size = size_add(size, qcomtee_msg_offset_align(u[i].b.size));
		if (size > MAX_BUFFER_SIZE)
			return -EINVAL;
	}

	/* QTEE requires inbound buffer size to be page aligned. */
	size = PAGE_ALIGN(size);

	oic->in_msg.size = size;
	oic->in_msg.addr = qcom_tzmem_alloc(tzmem_msg_pool, size, GFP_KERNEL);
	if (!oic->in_msg.addr)
		return -EINVAL;

	oic->out_msg.size = MAX_BUFFER_SIZE;
	oic->out_msg.addr =
		qcom_tzmem_alloc(tzmem_msg_pool, MAX_BUFFER_SIZE, GFP_KERNEL);
	if (!oic->out_msg.addr) {
		qcom_tzmem_free(oic->in_msg.addr);

		return -EINVAL;
	}

	oic->in_msg_paddr = qcom_tzmem_to_phys(oic->in_msg.addr);
	oic->out_msg_paddr = qcom_tzmem_to_phys(oic->out_msg.addr);

	/* QTEE assume unused buffers are zeroed. */
	memzero_explicit(oic->in_msg.addr, oic->in_msg.size);
	memzero_explicit(oic->out_msg.addr, oic->out_msg.size);

	return 0;
}

void qcomtee_msg_buffers_free(struct qcomtee_object_invoke_ctx *oic)
{
	qcom_tzmem_free(oic->in_msg.addr);
	qcom_tzmem_free(oic->out_msg.addr);
}

int qcomtee_msg_buffers_init(void)
{
	struct qcom_tzmem_pool_config config = {
		.policy = QCOM_TZMEM_POLICY_STATIC,
		.initial_size = SZ_16M
	};

	tzmem_msg_pool = qcom_tzmem_pool_new(&config);
	if (IS_ERR(tzmem_msg_pool))
		return PTR_ERR(tzmem_msg_pool);

	return 0;
}

void qcomtee_msg_buffers_destroy(void)
{
	qcom_tzmem_pool_free(tzmem_msg_pool);
}
