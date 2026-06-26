// SPDX-License-Identifier: GPL-2.0

#include <linux/eventfd.h>

__rust_helper void rust_helper_eventfd_ctx_put(struct eventfd_ctx *ctx)
{
	eventfd_ctx_put(ctx);
}

__rust_helper void rust_helper_eventfd_signal(struct eventfd_ctx *ctx)
{
	eventfd_signal(ctx);
}

__rust_helper struct eventfd_ctx *rust_helper_eventfd_ctx_fdget(int fd)
{
	return eventfd_ctx_fdget(fd);
}

__rust_helper struct eventfd_ctx *rust_helper_eventfd_ctx_fileget(struct file *file)
{
	return eventfd_ctx_fileget(file);
}

__rust_helper int rust_helper_eventfd_ctx_remove_wait_queue(
	struct eventfd_ctx *ctx, wait_queue_entry_t *wait, __u64 *cnt)
{
	return eventfd_ctx_remove_wait_queue(ctx, wait, cnt);
}

__rust_helper void rust_helper_eventfd_ctx_do_read(struct eventfd_ctx *ctx, __u64 *cnt)
{
	eventfd_ctx_do_read(ctx, cnt);
}
