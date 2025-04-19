// SPDX-License-Identifier: GPL-2.0

#include <linux/entry-common.h>
#include <linux/syscalls.h>
#include <asm/syscall.h>

int invoke_syscall_asm(void *table, struct pt_regs *regs, int scno);
__ADDRESSABLE(invoke_syscall_asm);

__visible void invoke_syscall(void *table, struct pt_regs *regs, int scno)
{
	int ret;

	scno = syscall_enter_from_user_mode(regs, scno);
	/* When tracing syscall -1 means "skip syscall" */
	if (scno < 0) {
		ret = 0;
		goto exit_save;
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
	syscall_set_return_value(current, regs, 0, ret);

	syscall_exit_to_user_mode(regs);
}
