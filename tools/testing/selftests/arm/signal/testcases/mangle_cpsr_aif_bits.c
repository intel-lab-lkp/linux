// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Limited
 *
 * Try to mangle the ucontext from inside a signal handler, mangling the
 * AIF bits in an illegal manner: this attempt must be spotted by Kernel
 * and the test case is expected to be terminated via SEGV.
 *
 */

#include "test_signals_utils.h"

static int mangle_invalid_cpsr_run(struct tdescr *td, siginfo_t *si,
				   ucontext_t *uc)
{

	/*
	 * This config should trigger a SIGSEGV by Kernel when it checks
	 * the sigframe consistency in valid_user_regs() routine.
	 */
	uc->uc_mcontext.arm_cpsr |= PSR_A_BIT | PSR_I_BIT | PSR_F_BIT;

	return 1;
}

struct tdescr tde = {
		.sanity_disabled = true,
		.name = "MANGLE_CPSR_INVALID_AIF_BITS",
		.descr = "Mangling uc_mcontext with INVALID AIF_BITS",
		.sig_trig = SIGUSR1,
		.sig_ok = SIGSEGV,
		.run = mangle_invalid_cpsr_run,
};
