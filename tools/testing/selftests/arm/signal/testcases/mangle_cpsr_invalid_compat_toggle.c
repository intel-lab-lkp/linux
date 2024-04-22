// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 ARM Limited
 *
 * Try to mangle the ucontext from inside a signal handler, toggling
 * the execution state bit: this attempt must be spotted by Kernel and
 * the test case is expected to be terminated via SEGV.
 */

#include "test_signals_utils.h"

static int mangle_invalid_cpsr_run(struct tdescr *td, siginfo_t *si,
				   ucontext_t *uc)
{

	/* This config should trigger a SIGSEGV by Kernel */
	uc->uc_mcontext.arm_cpsr ^= MODE32_BIT;

	return 1;
}

struct tdescr tde = {
		.sanity_disabled = true,
		.name = "MANGLE_CPSR_INVALID_STATE_TOGGLE",
		.descr = "Mangling uc_mcontext with INVALID STATE_TOGGLE",
		.sig_trig = SIGUSR1,
		.sig_ok = SIGSEGV,
		.run = mangle_invalid_cpsr_run,
};
