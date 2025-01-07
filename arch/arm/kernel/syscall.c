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

int invoke_syscall_trace_asm(void *table, struct pt_regs *regs, int scno);
__ADDRESSABLE(invoke_syscall_trace_asm);

__visible void invoke_syscall_trace(void *table, struct pt_regs *regs)
{
	int scno;
	int ret;

	scno = syscall_trace_enter(regs);
	if (scno == -1)
		goto trace_exit_nosave;

	if (scno < NR_syscalls) {
		ret = invoke_syscall_trace_asm(table, regs, scno);
		goto trace_exit_save;
	}

	if (scno >= __ARM_NR_BASE) {
		ret = arm_syscall(scno, regs);
		goto trace_exit_save;
	}

	ret = sys_ni_syscall();

trace_exit_save:
	/* Save return value from syscall */
	regs->ARM_r0 = ret;

trace_exit_nosave:
	local_irq_enable();
	syscall_trace_exit(regs);
}
