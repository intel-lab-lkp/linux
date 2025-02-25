// SPDX-License-Identifier: GPL-2.0

#include <linux/entry-common.h>
#include <linux/syscalls.h>
#include <asm/syscall.h>

int invoke_syscall_asm(void *table, struct pt_regs *regs, int scno);
__ADDRESSABLE(invoke_syscall_asm);

__visible void invoke_syscall(void *table, struct pt_regs *regs, int scno)
{
	int ret;

local_restart:
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

	/*
	 * Handle local restart: this means that when generic entry
	 * calls arch_do_signal_or_restart() because a signal to
	 * restart the syscall arrived while processing a system call,
	 * we set these flags for the thread so that we don't even
	 * exit the kernel, we just restart right here and clear
	 * the restart condition.
	 *
	 * This is done because of signal race issues on ARM.
	 */
	if (test_thread_flag(TIF_LOCAL_RESTART)) {
		if (test_thread_flag(TIF_LOCAL_RESTART_BLOCK)) {
			scno = __NR_restart_syscall - __NR_SYSCALL_BASE;
			/* Make this change visible to tracers */
			task_thread_info(current)->abi_syscall = scno;
			clear_thread_flag(TIF_LOCAL_RESTART_BLOCK);
		}
		clear_thread_flag(TIF_LOCAL_RESTART);
		goto local_restart;
	}
}
