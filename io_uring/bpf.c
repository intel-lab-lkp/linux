// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2024 Red Hat */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <uapi/linux/io_uring.h>
#include "io_uring.h"
#include "uring_bpf.h"

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
