/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef LINUX_TASK_SHARED_H
#define LINUX_TASK_SHARED_H

/*
 * Per task user-kernel mapped structure
 */

/*
 * Option to request allocation of struct task_sharedinfo shared structure,
 * used for sharing per thread information between userspace and kernel.
 */
#define TASK_SHAREDINFO 1

struct task_sharedinfo {
		volatile unsigned short sched_delay;
};
#endif
