/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MSHARE_H_
#define _LINUX_MSHARE_H_

#include <linux/types.h>

struct task_struct;

#ifdef CONFIG_MSHARE

void exit_mshare(struct task_struct *task);
#define mshare_init_task(task) INIT_LIST_HEAD(&(task)->mshare_mem)

#else

static inline void exit_mshare(struct task_struct *task)
{
}
static inline void mshare_init_task(struct task_struct *task)
{
}

#endif

#endif /* _LINUX_MSHARE_H_ */
