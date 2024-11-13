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

/*
 * 'sched_delay'  values:
 * TASK_PREEMPT_DELAY_REQ - application sets to request preemption delay.
 * TASK_PREEMPT_DELAY_GRANTED - set by kernel if granted extended time on cpu.
 * TASK_PREEMPT_DELAY_DENIED- set by kernel if not granted because the
 *     application requested preemption delay again within the extended time.
 */
#define TASK_PREEMPT_DELAY_REQ		1
#define TASK_PREEMPT_DELAY_GRANTED	2
#define TASK_PREEMPT_DELAY_DENIED	3
#endif
