// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#include <linux/cpu_pm.h>
#include <linux/ftrace.h>
#include <linux/thread_info.h>
#include <linux/syscore_ops.h>
#include <asm/csr.h>
#include <asm/suspend.h>
#include <asm/switch_to.h>
#include <asm/vector.h>

void suspend_save_csrs(struct suspend_context *context)
{
	context->scratch = csr_read(CSR_SCRATCH);
	context->tvec = csr_read(CSR_TVEC);
	context->ie = csr_read(CSR_IE);

	/*
	 * No need to save/restore IP CSR (i.e. MIP or SIP) because:
	 *
	 * 1. For no-MMU (M-mode) kernel, the bits in MIP are set by
	 *    external devices (such as interrupt controller, timer, etc).
	 * 2. For MMU (S-mode) kernel, the bits in SIP are set by
	 *    M-mode firmware and external devices (such as interrupt
	 *    controller, etc).
	 */

#ifdef CONFIG_MMU
	context->satp = csr_read(CSR_SATP);
#endif
}

void suspend_restore_csrs(struct suspend_context *context)
{
	csr_write(CSR_SCRATCH, context->scratch);
	csr_write(CSR_TVEC, context->tvec);
	csr_write(CSR_IE, context->ie);

#ifdef CONFIG_MMU
	csr_write(CSR_SATP, context->satp);
#endif
}

int cpu_suspend(unsigned long arg,
		int (*finish)(unsigned long arg,
			      unsigned long entry,
			      unsigned long context))
{
	int rc = 0;
	struct suspend_context context = { 0 };

	/* Finisher should be non-NULL */
	if (!finish)
		return -EINVAL;

	/* Save additional CSRs*/
	suspend_save_csrs(&context);

	/*
	 * Function graph tracer state gets incosistent when the kernel
	 * calls functions that never return (aka finishers) hence disable
	 * graph tracing during their execution.
	 */
	pause_graph_tracing();

	/* Save context on stack */
	if (__cpu_suspend_enter(&context)) {
		/* Call the finisher */
		rc = finish(arg, __pa_symbol(__cpu_resume_enter),
			    (ulong)&context);

		/*
		 * Should never reach here, unless the suspend finisher
		 * fails. Successful cpu_suspend() should return from
		 * __cpu_resume_entry()
		 */
		if (!rc)
			rc = -EOPNOTSUPP;
	}

	/* Enable function graph tracer */
	unpause_graph_tracing();

	/* Restore additional CSRs */
	suspend_restore_csrs(&context);

	return rc;
}

static int riscv_cpu_suspend(void)
{
	struct task_struct *cur_task = get_current();
	struct pt_regs *regs = task_pt_regs(cur_task);

	if (has_fpu()) {
		if (unlikely(regs->status & SR_SD))
			fstate_save(cur_task, regs);
	}
	if (has_vector()) {
		if (unlikely(regs->status & SR_SD))
			riscv_v_vstate_save(cur_task, regs);
	}

	return 0;
}

static void riscv_cpu_resume(void)
{
	struct task_struct *cur_task = get_current();
	struct pt_regs *regs = task_pt_regs(cur_task);

	if (has_fpu())
		fstate_restore(cur_task, regs);
	if (has_vector())
		riscv_v_vstate_restore(cur_task, regs);
}

static struct syscore_ops riscv_cpu_syscore_ops = {
	.suspend	= riscv_cpu_suspend,
	.resume		= riscv_cpu_resume,
};

static int __init riscv_cpu_suspend_init(void)
{
	register_syscore_ops(&riscv_cpu_syscore_ops);
	return 0;
}
arch_initcall(riscv_cpu_suspend_init);
