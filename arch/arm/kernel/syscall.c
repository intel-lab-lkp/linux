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

	return 0;
}

int invoke_syscall_trace_asm(void *table, struct pt_regs *regs, int scno, void *retp);
__ADDRESSABLE(invoke_syscall_trace_asm);

__visible int invoke_syscall_trace(void *table, struct pt_regs *regs, int scno, void *retp)
{
	if (scno < NR_syscalls)
		/* Doing this with return makes sure the stack gets pop:ed */
		return invoke_syscall_trace_asm(table, regs, scno, retp);

	return 0;
}
