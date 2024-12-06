/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_ARM64_ENTRY_COMMON_H
#define _ASM_ARM64_ENTRY_COMMON_H

#include <linux/thread_info.h>

#include <asm/daifflags.h>
#include <asm/fpsimd.h>
#include <asm/mte.h>
#include <asm/stacktrace.h>

enum ptrace_syscall_dir {
	PTRACE_SYSCALL_ENTER = 0,
	PTRACE_SYSCALL_EXIT,
};

#define ARCH_SYSCALL_WORK_EXIT (SYSCALL_WORK_SECCOMP | SYSCALL_WORK_SYSCALL_EMU)
#define ARCH_EXIT_TO_USER_MODE_WORK (_TIF_MTE_ASYNC_FAULT | _TIF_FOREIGN_FPSTATE)

static __always_inline void arch_exit_to_user_mode_work(struct pt_regs *regs,
							unsigned long ti_work)
{
	if (ti_work & _TIF_MTE_ASYNC_FAULT) {
		clear_thread_flag(TIF_MTE_ASYNC_FAULT);
		send_sig_fault(SIGSEGV, SEGV_MTEAERR, (void __user *)NULL, current);
	}

	if (ti_work & _TIF_FOREIGN_FPSTATE)
		fpsimd_restore_current_state();
}

#define arch_exit_to_user_mode_work arch_exit_to_user_mode_work

static inline void arch_exit_to_user_mode_prepare(struct pt_regs *regs,
						  unsigned long ti_work)
{
	local_daif_mask();
}

#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare

static inline bool arch_irqentry_exit_need_resched(void)
{
	/*
	 * DAIF.DA are cleared at the start of IRQ/FIQ handling, and when GIC
	 * priority masking is used the GIC irqchip driver will clear DAIF.IF
	 * using gic_arch_enable_irqs() for normal IRQs. If anything is set in
	 * DAIF we must have handled an NMI, so skip preemption.
	 */
	if (system_uses_irq_prio_masking() && read_sysreg(daif))
		return false;

	/*
	 * Preempting a task from an IRQ means we leave copies of PSTATE
	 * on the stack. cpufeature's enable calls may modify PSTATE, but
	 * resuming one of these preempted tasks would undo those changes.
	 *
	 * Only allow a task to be preempted once cpufeatures have been
	 * enabled.
	 */
	if (!system_capabilities_finalized())
		return false;

	return true;
}

#define arch_irqentry_exit_need_resched arch_irqentry_exit_need_resched

static inline int arch_ptrace_report_syscall_entry(struct pt_regs *regs)
{
	unsigned long saved_reg;
	int regno, ret;

	/*
	 * We have some ABI weirdness here in the way that we handle syscall
	 * exit stops because we indicate whether or not the stop has been
	 * signalled from syscall entry or syscall exit by clobbering a general
	 * purpose register (ip/r12 for AArch32, x7 for AArch64) in the tracee
	 * and restoring its old value after the stop. This means that:
	 *
	 * - Any writes by the tracer to this register during the stop are
	 *   ignored/discarded.
	 *
	 * - The actual value of the register is not available during the stop,
	 *   so the tracer cannot save it and restore it later.
	 *
	 * - Syscall stops behave differently to seccomp and pseudo-step traps
	 *   (the latter do not nobble any registers).
	 */
	regno = (is_compat_task() ? 12 : 7);
	saved_reg = regs->regs[regno];
	regs->regs[regno] = PTRACE_SYSCALL_ENTER;

	ret = ptrace_report_syscall_entry(regs);
	if (ret)
		forget_syscall(regs);

	regs->regs[regno] = saved_reg;

	return ret;
}

#define arch_ptrace_report_syscall_entry arch_ptrace_report_syscall_entry

static inline void arch_ptrace_report_syscall_exit(struct pt_regs *regs,
						   int step)
{
	unsigned long saved_reg;
	int regno;

	/* See comment for arch_ptrace_report_syscall_entry() */
	regno = (is_compat_task() ? 12 : 7);
	saved_reg = regs->regs[regno];
	regs->regs[regno] = PTRACE_SYSCALL_EXIT;

	if (!test_thread_flag(TIF_SINGLESTEP)) {
		ptrace_report_syscall_exit(regs, 0);
		regs->regs[regno] = saved_reg;
	} else {
		regs->regs[regno] = saved_reg;

		/*
		 * Signal a pseudo-step exception since we are stepping but
		 * tracer modifications to the registers may have rewound the
		 * state machine.
		 */
		ptrace_report_syscall_exit(regs, 1);
	}
}

#define arch_ptrace_report_syscall_exit arch_ptrace_report_syscall_exit

#endif /* _ASM_ARM64_ENTRY_COMMON_H */
