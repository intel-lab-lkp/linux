// SPDX-License-Identifier: GPL-2.0

#include <linux/sched/task.h>

struct task_struct *rust_helper_get_current(void)
{
	return current;
}
