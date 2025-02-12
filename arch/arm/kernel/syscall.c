// SPDX-License-Identifier: GPL-2.0

#include <linux/syscalls.h>
#include <asm/syscall.h>

static inline bool has_syscall_work(unsigned long flags)
{
	return unlikely(flags & _TIF_SYSCALL_WORK);
}

int invoke_syscall_asm(void *table, struct pt_regs *regs, int scno);
__ADDRESSABLE(invoke_syscall_asm);

__visible int invoke_syscall(void *table, struct pt_regs *regs, int scno)
{
	unsigned long flags = read_thread_flags();
	int ret;

	if (has_syscall_work(flags)) {
		scno = syscall_trace_enter(regs);
		if (scno == -1)
			goto trace_exit_nosave;
	}

	if (scno < NR_syscalls) {
		ret = invoke_syscall_asm(table, regs, scno);
		goto exit_save;
	}

	if (scno >= __ARM_NR_BASE) {
		ret = arm_syscall(scno, regs);
		goto exit_save;
	}

	ret = sys_ni_syscall();

exit_save:
	/* Save return value from syscall */
	regs->ARM_r0 = ret;
	if (!has_syscall_work(flags))
		return 0;

trace_exit_nosave:
	local_irq_enable();
	syscall_trace_exit(regs);
	return 1;
}
