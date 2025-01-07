// SPDX-License-Identifier: GPL-2.0

#include <linux/syscalls.h>
#include <asm/syscall.h>

int invoke_syscall_asm(void *table, struct pt_regs *regs, int scno, void *retp);
__ADDRESSABLE(invoke_syscall_asm);

__visible int invoke_syscall(void *table, struct pt_regs *regs, int scno, void *retp)
{
	if (scno < NR_syscalls)
		/* Doing this with return makes sure the stack gets pop:ed */
		return invoke_syscall_asm(table, regs, scno, retp);

	if (scno >= __ARM_NR_BASE)
		return arm_syscall(scno, regs);

	return sys_ni_syscall();
}

int invoke_syscall_trace_asm(void *table, struct pt_regs *regs, int scno, void *retp);
__ADDRESSABLE(invoke_syscall_trace_asm);

__visible int invoke_syscall_trace(void *table, struct pt_regs *regs, void *retp)
{
	int scno;

	scno = syscall_trace_enter(regs);
	if (scno == -1)
		return -1;

	if (scno < NR_syscalls)
		/* Doing this with return makes sure the stack gets pop:ed */
		return invoke_syscall_trace_asm(table, regs, scno, retp);

	if (scno >= __ARM_NR_BASE)
		return arm_syscall(scno, regs);

	return sys_ni_syscall();
}
