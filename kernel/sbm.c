// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Huawei Technologies Duesseldorf GmbH
 *
 * Author: Petr Tesarik <petr.tesarik1@huawei-partners.com>
 *
 * SandBox Mode (SBM) public API and generic functions.
 */

#include <linux/export.h>
#include <linux/sbm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

struct sbm_buf *sbm_alloc_buf(struct sbm *sbm, size_t size)
{
	struct sbm_buf *buf;

	if (sbm->error)
		return NULL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		sbm->error = -ENOMEM;
		goto out;
	}
	buf->sbm_ptr = vzalloc(size);
	if (!buf->sbm_ptr) {
		sbm->error = -ENOMEM;
		goto out;
	}
	buf->size = size;

out:
	return buf;
}
EXPORT_SYMBOL(sbm_alloc_buf);

/* Free a buffer list. */
static void sbm_free_buf_list(const struct sbm_buf *buf)
{
	const struct sbm_buf *nextbuf;

	while (buf) {
		vfree(buf->sbm_ptr);
		nextbuf = buf->next;
		kfree(buf);
		buf = nextbuf;
	}
}

int sbm_init(struct sbm *sbm)
{
	memset(sbm, 0, sizeof(*sbm));

	sbm->error = arch_sbm_init(sbm);
	if (sbm->error)
		return sbm->error;

	return 0;
}
EXPORT_SYMBOL(sbm_init);

void sbm_destroy(struct sbm *sbm)
{
	sbm_free_buf_list(sbm->input);
	sbm_free_buf_list(sbm->output);
	sbm_free_buf_list(sbm->io);
	arch_sbm_destroy(sbm);
}
EXPORT_SYMBOL(sbm_destroy);

/* Copy input buffers into a sandbox. */
static int sbm_copy_in(struct sbm *sbm)
{
	const struct sbm_buf *buf;
	int err = 0;

	for (buf = sbm->input; buf; buf = buf->next) {
		err = arch_sbm_map_readonly(sbm, buf);
		if (err)
			return err;
		memcpy(buf->sbm_ptr, buf->kern_ptr, buf->size);
	}

	for (buf = sbm->io; buf; buf = buf->next) {
		err = arch_sbm_map_writable(sbm, buf);
		if (err)
			return err;
		memcpy(buf->sbm_ptr, buf->kern_ptr, buf->size);
	}

	for (buf = sbm->output; buf; buf = buf->next) {
		err = arch_sbm_map_writable(sbm, buf);
		if (err)
			return err;
	}

	return 0;
}

/* Copy output buffers out of a sandbox. */
static void sbm_copy_out(struct sbm *sbm)
{
	const struct sbm_buf *buf;

	for (buf = sbm->output; buf; buf = buf->next)
		memcpy(buf->kern_ptr, buf->sbm_ptr, buf->size);
	for (buf = sbm->io; buf; buf = buf->next)
		memcpy(buf->kern_ptr, buf->sbm_ptr, buf->size);
}

int sbm_exec(struct sbm *sbm, sbm_func func, void *args)
{
	int ret;

	if (sbm->error)
		return sbm->error;

	sbm->error = sbm_copy_in(sbm);
	if (sbm->error)
		return sbm->error;

	ret = arch_sbm_exec(sbm, func, args);
	if (sbm->error)
		return sbm->error;

	sbm_copy_out(sbm);

	return ret;
}
EXPORT_SYMBOL(sbm_exec);
