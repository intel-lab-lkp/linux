// SPDX-License-Identifier: GPL-2.0

#include <linux/ptrace.h>
#include <asm/bugs.h>
#include <asm/traps.h>

enum cp_error_code {
	CP_EC        = (1 << 15) - 1,

	CP_RET       = 1,
	CP_IRET      = 2,
	CP_ENDBR     = 3,
	CP_RSTRORSSP = 4,
	CP_SETSSBSY  = 5,

	CP_ENCL	     = 1 << 15,
};

static const char cp_err[][10] = {
	[0] = "unknown",
	[1] = "near ret",
	[2] = "far/iret",
	[3] = "endbranch",
	[4] = "rstorssp",
	[5] = "setssbsy",
};

static const char *cp_err_string(unsigned long error_code)
{
	unsigned int cpec = error_code & CP_EC;

	if (cpec >= ARRAY_SIZE(cp_err))
		cpec = 0;
	return cp_err[cpec];
}

static void do_unexpected_cp(struct pt_regs *regs, unsigned long error_code)
{
	WARN_ONCE(1, "Unexpected %s #CP, error_code: %s\n",
		  user_mode(regs) ? "user mode" : "kernel mode",
		  cp_err_string(error_code));
}

static DEFINE_RATELIMIT_STATE(cpf_rate, DEFAULT_RATELIMIT_INTERVAL,
			      DEFAULT_RATELIMIT_BURST);

static void do_user_cp_fault(struct pt_regs *regs, unsigned long error_code)
{
	struct task_struct *tsk;
	unsigned long ssp;

	/*
	 * An exception was just taken from userspace. Since interrupts are disabled
	 * here, no scheduling should have messed with the registers yet and they
	 * will be whatever is live in userspace. So read the SSP before enabling
	 * interrupts so locking the fpregs to do it later is not required.
	 */
	rdmsrl(MSR_IA32_PL3_SSP, ssp);

	cond_local_irq_enable(regs);

	tsk = current;
	tsk->thread.error_code = error_code;
	tsk->thread.trap_nr = X86_TRAP_CP;

	/* Ratelimit to prevent log spamming. */
	if (show_unhandled_signals && unhandled_signal(tsk, SIGSEGV) &&
	    __ratelimit(&cpf_rate)) {
		pr_emerg("%s[%d] control protection ip:%lx sp:%lx ssp:%lx error:%lx(%s)%s",
			 tsk->comm, task_pid_nr(tsk),
			 regs->ip, regs->sp, ssp, error_code,
			 cp_err_string(error_code),
			 error_code & CP_ENCL ? " in enclave" : "");
		print_vma_addr(KERN_CONT " in ", regs->ip);
		pr_cont("\n");
	}

	force_sig_fault(SIGSEGV, SEGV_CPERR, (void __user *)0);
	cond_local_irq_disable(regs);
}

static __ro_after_init bool ibt_fatal = true;

/*
 * The WFE (WAIT_FOR_ENDBRANCH) bit in the augmented CS of FRED stack frame is
 * set to 1 in missing-ENDBRANCH #CP exceptions.
 *
 * If the WFE bit is left as 1, the CPU will generate another missing-ENDBRANCH
 * #CP because the indirect branch tracker will be set in the WAIT_FOR_ENDBRANCH
 * state upon completion of the following ERETS instruction and the CPU will
 * restart from the IP that just caused a previous missing-ENDBRANCH #CP.
 *
 * Clear the WFE bit to avoid dead looping due to the above reason.
 */
static void ibt_clear_fred_wfe(struct pt_regs *regs)
{
	if (cpu_feature_enabled(X86_FEATURE_FRED))
		regs->fred_cs.wfe = 0;
}

static void do_kernel_cp_fault(struct pt_regs *regs, unsigned long error_code)
{
	if ((error_code & CP_EC) != CP_ENDBR) {
		do_unexpected_cp(regs, error_code);
		return;
	}

	if (unlikely(regs->ip == (unsigned long)&ibt_selftest_noendbr)) {
		regs->ax = 0;
		ibt_clear_fred_wfe(regs);
		return;
	}

	pr_err("Missing ENDBR: %pS\n", (void *)instruction_pointer(regs));
	if (!ibt_fatal) {
		printk(KERN_DEFAULT CUT_HERE);
		__warn(__FILE__, __LINE__, (void *)regs->ip, TAINT_WARN, regs, NULL);
		ibt_clear_fred_wfe(regs);
		return;
	}
	BUG();
}

static int __init ibt_setup(char *str)
{
	if (!strcmp(str, "off"))
		setup_clear_cpu_cap(X86_FEATURE_IBT);

	if (!strcmp(str, "warn"))
		ibt_fatal = false;

	return 1;
}

__setup("ibt=", ibt_setup);

DEFINE_IDTENTRY_ERRORCODE(exc_control_protection)
{
	if (user_mode(regs)) {
		if (cpu_feature_enabled(X86_FEATURE_USER_SHSTK))
			do_user_cp_fault(regs, error_code);
		else
			do_unexpected_cp(regs, error_code);
	} else {
		if (cpu_feature_enabled(X86_FEATURE_IBT))
			do_kernel_cp_fault(regs, error_code);
		else
			do_unexpected_cp(regs, error_code);
	}
}
