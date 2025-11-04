// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Red Hat */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <uapi/linux/io_uring.h>
#include "io_uring.h"
#include "uring_bpf.h"

static DEFINE_MUTEX(uring_bpf_ctx_lock);
static LIST_HEAD(uring_bpf_ctx_list);

int io_uring_bpf_issue(struct io_kiocb *req, unsigned int issue_flags)
{
	return -ECANCELED;
}

int io_uring_bpf_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	return -EOPNOTSUPP;
}

void io_uring_bpf_fail(struct io_kiocb *req)
{
}

void io_uring_bpf_cleanup(struct io_kiocb *req)
{
}

void uring_bpf_add_ctx(struct io_ring_ctx *ctx)
{
	guard(mutex)(&uring_bpf_ctx_lock);
	list_add(&ctx->bpf_node, &uring_bpf_ctx_list);
}

void uring_bpf_del_ctx(struct io_ring_ctx *ctx)
{
	guard(mutex)(&uring_bpf_ctx_lock);
	list_del(&ctx->bpf_node);
}
