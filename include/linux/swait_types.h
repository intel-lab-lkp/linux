/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAIT_TYPES_H
#define _LINUX_SWAIT_TYPES_H

#include <linux/list.h>
#include <linux/spinlock_types.h>

struct task_struct;

struct swait_queue_head {
	raw_spinlock_t		lock;
	struct list_head	task_list;
};

struct swait_queue {
	struct task_struct	*task;
	struct list_head	task_list;
};

#endif /* _LINUX_SWAIT_TYPES_H */
