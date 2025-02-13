// SPDX-License-Identifier: GPL-2.0

#include <linux/audit.h>
#include <linux/entry-common.h>
#include "common.h"

#define CREATE_TRACE_POINTS
#include <trace/events/syscalls.h>

static inline void syscall_enter_audit(struct pt_regs *regs, long syscall)
{
	if (unlikely(audit_context())) {
		unsigned long args[6];

		syscall_get_arguments(current, regs, args);
		audit_syscall_entry(syscall, args[0], args[1], args[2], args[3]);
	}
}

long syscall_trace_enter(struct pt_regs *regs, long syscall,
				unsigned long work)
{
	long ret = 0;

	/*
	 * Handle Syscall User Dispatch.  This must comes first, since
	 * the ABI here can be something that doesn't make sense for
	 * other syscall_work features.
	 */
	if (work & SYSCALL_WORK_SYSCALL_USER_DISPATCH) {
		if (syscall_user_dispatch(regs))
			return -1L;
	}

	/* Handle ptrace */
	if (work & (SYSCALL_WORK_SYSCALL_TRACE | SYSCALL_WORK_SYSCALL_EMU)) {
		ret = ptrace_report_syscall_entry(regs);
		if (ret || (work & SYSCALL_WORK_SYSCALL_EMU))
			return -1L;
	}

	/* Do seccomp after ptrace, to catch any tracer changes. */
	if (work & SYSCALL_WORK_SECCOMP) {
		ret = __secure_computing();
		if (ret == -1L)
			return ret;
	}

	/* Either of the above might have changed the syscall number */
	syscall = syscall_get_nr(current, regs);

	if (unlikely(work & SYSCALL_WORK_SYSCALL_TRACEPOINT)) {
		trace_sys_enter(regs, syscall);
		/*
		 * Probes or BPF hooks in the tracepoint may have changed the
		 * system call number as well.
		 */
		syscall = syscall_get_nr(current, regs);
	}

	syscall_enter_audit(regs, syscall);

	return ret ? : syscall;
}

noinstr void syscall_enter_from_user_mode_prepare(struct pt_regs *regs)
{
	enter_from_user_mode(regs);
	instrumentation_begin();
	local_irq_enable();
	instrumentation_end();
}

/*
 * If SYSCALL_EMU is set, then the only reason to report is when
 * SINGLESTEP is set (i.e. PTRACE_SYSEMU_SINGLESTEP).  This syscall
 * instruction has been already reported in syscall_enter_from_user_mode().
 */
static inline bool report_single_step(unsigned long work)
{
	if (work & SYSCALL_WORK_SYSCALL_EMU)
		return false;

	return work & SYSCALL_WORK_SYSCALL_EXIT_TRAP;
}

static void syscall_exit_work(struct pt_regs *regs, unsigned long work)
{
	bool step;

	/*
	 * If the syscall was rolled back due to syscall user dispatching,
	 * then the tracers below are not invoked for the same reason as
	 * the entry side was not invoked in syscall_trace_enter(): The ABI
	 * of these syscalls is unknown.
	 */
	if (work & SYSCALL_WORK_SYSCALL_USER_DISPATCH) {
		if (unlikely(current->syscall_dispatch.on_dispatch)) {
			current->syscall_dispatch.on_dispatch = false;
			return;
		}
	}

	audit_syscall_exit(regs);

	if (work & SYSCALL_WORK_SYSCALL_TRACEPOINT)
		trace_sys_exit(regs, syscall_get_return_value(current, regs));

	step = report_single_step(work);
	if (step || work & SYSCALL_WORK_SYSCALL_TRACE)
		ptrace_report_syscall_exit(regs, step);
}

/*
 * Syscall specific exit to user mode preparation. Runs with interrupts
 * enabled.
 */
static void syscall_exit_to_user_mode_prepare(struct pt_regs *regs)
{
	unsigned long work = READ_ONCE(current_thread_info()->syscall_work);
	unsigned long nr = syscall_get_nr(current, regs);

	CT_WARN_ON(ct_state() != CT_STATE_KERNEL);

	if (IS_ENABLED(CONFIG_PROVE_LOCKING)) {
		if (WARN(irqs_disabled(), "syscall %lu left IRQs disabled", nr))
			local_irq_enable();
	}

	rseq_syscall(regs);

	/*
	 * Do one-time syscall specific work. If these work items are
	 * enabled, we want to run them exactly once per syscall exit with
	 * interrupts enabled.
	 */
	if (unlikely(work & SYSCALL_WORK_EXIT))
		syscall_exit_work(regs, work);
}

static __always_inline void __syscall_exit_to_user_mode_work(struct pt_regs *regs)
{
	syscall_exit_to_user_mode_prepare(regs);
	local_irq_disable_exit_to_user();
	exit_to_user_mode_prepare(regs);
}

void syscall_exit_to_user_mode_work(struct pt_regs *regs)
{
	__syscall_exit_to_user_mode_work(regs);
}

__visible noinstr void syscall_exit_to_user_mode(struct pt_regs *regs)
{
	instrumentation_begin();
	__syscall_exit_to_user_mode_work(regs);
	instrumentation_end();
	exit_to_user_mode();
}
