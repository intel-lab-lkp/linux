/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _LINUX_IO_URING_H
#define _LINUX_IO_URING_H

#include <linux/io_uring_types.h>
#include <linux/sched.h>
#include <linux/xarray.h>
#include <uapi/linux/io_uring.h>

/*
 * Terminate the request if either of these conditions are true:
 *
 * 1) It's being executed by the original task, but that task is marked
 *    with PF_EXITING as it's exiting.
 * 2) PF_KTHREAD is set, in which case the invoker of the task_work is
 *    our fallback task_work.
 */
static inline bool io_should_terminate_tw(struct io_ring_ctx *ctx)
{
	return (current->flags & (PF_KTHREAD | PF_EXITING)) || percpu_ref_is_dying(&ctx->refs);
}

#if defined(CONFIG_IO_URING)
void __io_uring_cancel(bool cancel_all);
void __io_uring_free(struct task_struct *tsk);
void io_uring_unreg_ringfd(void);
const char *io_uring_get_opcode(u8 opcode);
bool io_is_uring_fops(struct file *file);

static inline void io_uring_files_cancel(void)
{
	if (current->io_uring)
		__io_uring_cancel(false);
}
static inline void io_uring_task_cancel(void)
{
	if (current->io_uring)
		__io_uring_cancel(true);
}
static inline void io_uring_free(struct task_struct *tsk)
{
	if (tsk->io_uring)
		__io_uring_free(tsk);
}
#else
static inline void io_uring_task_cancel(void)
{
}
static inline void io_uring_files_cancel(void)
{
}
static inline void io_uring_free(struct task_struct *tsk)
{
}
static inline const char *io_uring_get_opcode(u8 opcode)
{
	return "";
}
static inline bool io_is_uring_fops(struct file *file)
{
	return false;
}
#endif

#endif
