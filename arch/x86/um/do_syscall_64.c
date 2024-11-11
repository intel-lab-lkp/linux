// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <kern_util.h>
#include <sysdep/syscalls.h>
#include <os.h>

__visible void do_syscall_64(struct pt_regs *regs)
{
	int syscall;

	syscall = PT_SYSCALL_NR(regs->regs.gp);
	UPT_SYSCALL_NR(&regs->regs) = syscall;

	pr_debug("syscall(%d) (current=%lx) (fn=%lx)\n",
		 syscall, (unsigned long)current,
		 (unsigned long)sys_call_table[syscall]);

	if (likely(syscall < NR_syscalls)) {
		PT_REGS_SET_SYSCALL_RETURN(regs,
				EXECUTE_SYSCALL(syscall, regs));
	}

	pr_debug("syscall(%d) --> %lx\n", syscall,
		regs->regs.gp[HOST_AX]);

	PT_REGS_SYSCALL_RET(regs) = regs->regs.gp[HOST_AX];

	/* force do_signal() --> is_syscall() */
	set_thread_flag(TIF_SIGPENDING);
	interrupt_end();

	/* execve succeeded */
	if (syscall == __NR_execve && regs->regs.gp[HOST_AX] == 0)
		userspace(&current->thread.regs.regs);
}
