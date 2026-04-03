/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ALPHA_SYSCALL_H
#define _ASM_ALPHA_SYSCALL_H

#include <uapi/linux/audit.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

static inline int syscall_get_nr(struct task_struct *task,
				 struct pt_regs *regs)
{
	return regs->r0;
}

static inline void syscall_set_nr(struct task_struct *task,
				  struct pt_regs *regs, int nr)
{
	regs->r0 = nr;
}

static inline void syscall_rollback(struct task_struct *task,
				    struct pt_regs *regs)
{
	/* Alpha does not save the original syscall number separately. */
}

static inline long syscall_get_error(struct task_struct *task,
				     struct pt_regs *regs)
{
	return regs->r19 ? regs->r0 : 0;
}

static inline long syscall_get_return_value(struct task_struct *task,
					    struct pt_regs *regs)
{
	return regs->r0;
}

static inline void syscall_set_return_value(struct task_struct *task,
					    struct pt_regs *regs,
					    int error, long val)
{
	if (error) {
		regs->r0 = error;
		regs->r19 = 1;		/* a3: signal error */
	} else {
		regs->r0 = val;
		regs->r19 = 0;		/* a3: no error */
	}
}

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
	args[0] = regs->r16;		/* a0 */
	args[1] = regs->r17;		/* a1 */
	args[2] = regs->r18;		/* a2 */
	args[3] = regs->r19;		/* a3 */
	args[4] = regs->r20;		/* a4 */
	args[5] = regs->r21;		/* a5 */
}

static inline void syscall_set_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 const unsigned long *args)
{
	regs->r16 = args[0];		/* a0 */
	regs->r17 = args[1];		/* a1 */
	regs->r18 = args[2];		/* a2 */
	regs->r19 = args[3];		/* a3 */
	regs->r20 = args[4];		/* a4 */
	regs->r21 = args[5];		/* a5 */
}

static inline int syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_ALPHA;
}

#endif	/* _ASM_ALPHA_SYSCALL_H */
