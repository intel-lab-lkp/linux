/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scheduler internal task_work methods
 */
#ifndef _KERNEL_TASK_WORK_SCHED_H
#define _KERNEL_TASK_WORK_SCHED_H

#include <linux/task_work.h>
#include <linux/sched.h>

struct callback_head *
task_work_cancel_locked(struct task_struct *task, task_work_func_t func);

#endif
