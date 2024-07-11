/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RCUWAIT_TYPES_H_
#define _LINUX_RCUWAIT_TYPES_H_

#include <linux/sched.h>

/*
 * The only time @task is non-nil is when a user is blocked (or
 * checking if it needs to) on a condition, and reset as soon as we
 * know that the condition has succeeded and are awoken.
 */
struct rcuwait {
	struct task_struct __rcu *task;
};

#endif
