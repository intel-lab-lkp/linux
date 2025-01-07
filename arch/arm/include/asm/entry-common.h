/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_ARM_ENTRY_COMMON_H
#define _ASM_ARM_ENTRY_COMMON_H

#include <linux/thread_info.h>

#include <asm/stacktrace.h>

enum ptrace_syscall_dir {
	PTRACE_SYSCALL_ENTER = 0,
	PTRACE_SYSCALL_EXIT,
};

static inline unsigned long
arch_prepare_report_syscall_entry(struct pt_regs *regs)
{
	unsigned long ip;

	/*
	 * IP is used to denote syscall entry/exit:
	 * IP = 0 -> entry
	 */
	ip = regs->ARM_ip;
	regs->ARM_ip = PTRACE_SYSCALL_ENTER;

	return ip;
}
#define arch_prepare_report_syscall_entry arch_prepare_report_syscall_entry

static inline void
arch_post_report_syscall_entry(struct pt_regs *regs,
			       unsigned long saved_reg, long ret)
{
	regs->ARM_ip = saved_reg;
}
#define arch_post_report_syscall_entry arch_post_report_syscall_entry


static inline unsigned long
arch_prepare_report_syscall_exit(struct pt_regs *regs,
				 unsigned long work)
{
	unsigned long ip;

	/*
	 * IP is used to denote syscall entry/exit:
	 * IP = 1 -> exit
	 */
	ip = regs->ARM_ip;
	regs->ARM_ip = PTRACE_SYSCALL_EXIT;

	return ip;
}
#define arch_prepare_report_syscall_exit arch_prepare_report_syscall_exit

static inline void
arch_post_report_syscall_exit(struct pt_regs *regs,
			      unsigned long saved_reg,
			      unsigned long work)
{
	regs->ARM_ip = saved_reg;
}
#define arch_post_report_syscall_exit arch_post_report_syscall_exit

#endif /* _ASM_ARM_ENTRY_COMMON_H */
