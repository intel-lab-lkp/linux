/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_ARM64_ENTRY_COMMON_H
#define _ASM_ARM64_ENTRY_COMMON_H

#include <linux/sched/signal.h>

#include <asm/stacktrace.h>
#include <asm/fpsimd.h>
#include <asm/mte.h>
#include <asm/daifflags.h>

#define ARCH_EXIT_TO_USER_MODE_WORK (_TIF_MTE_ASYNC_FAULT | _TIF_FOREIGN_FPSTATE)

static __always_inline void arch_enter_from_user_mode(struct pt_regs *regs)
{
	mte_disable_tco_entry(current);
}

#define arch_enter_from_user_mode arch_enter_from_user_mode

static inline void arch_exit_to_user_mode_work(struct pt_regs *regs,
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
	mte_check_tfsr_exit();
}

#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare

static __always_inline void arch_enter_from_kernel_mode(struct pt_regs *regs)
{
	mte_check_tfsr_entry();
	mte_disable_tco_entry(current);
}

#define arch_enter_from_kernel_mode arch_enter_from_kernel_mode

static __always_inline void arch_exit_to_kernel_mode_prepare(struct pt_regs *regs)
{
	mte_check_tfsr_exit();
}

#define arch_exit_to_kernel_mode_prepare arch_exit_to_kernel_mode_prepare

#endif /* _ASM_ARM64_ENTRY_COMMON_H */
