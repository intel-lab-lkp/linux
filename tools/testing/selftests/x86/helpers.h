// SPDX-License-Identifier: GPL-2.0-only
#ifndef __SELFTESTS_X86_HELPERS_H
#define __SELFTESTS_X86_HELPERS_H

#include <signal.h>
#include <string.h>
#include <stdbool.h>

#include <asm/processor-flags.h>

#include "kselftest.h"

static inline unsigned long get_eflags(void)
{
#ifdef __x86_64__
	return __builtin_ia32_readeflags_u64();
#else
	return __builtin_ia32_readeflags_u32();
#endif
}

static inline void set_eflags(unsigned long eflags)
{
#ifdef __x86_64__
	__builtin_ia32_writeeflags_u64(eflags);
#else
	__builtin_ia32_writeeflags_u32(eflags);
#endif
}

static inline void sethandler(int sig, void (*handler)(int, siginfo_t *, void *), int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | flags;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, 0))
		ksft_exit_fail_msg("sigaction failed");
}

static inline void clearhandler(int sig)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, 0))
		ksft_exit_fail_msg("sigaction failed");
}

static inline void fred_handler(int sig, siginfo_t *info, void *ctx_void)
{
}

static inline bool is_fred_enabled(void)
{
	unsigned short gs_val;

	sethandler(SIGTRAP, fred_handler, 0);

	/*
	 * Distinguish IDT and FRED mode by loading GS with a non-zero RPL and
	 * triggering an exception:
	 * IDT (IRET) clears RPL bits of NULL selectors.
	 * FRED (ERETU) preserves them.
	 *
	 * If GS is loaded with 3 (Index=0, RPL=3), trigger an exception:
	 * IDT should restore GS as 0.
	 * FRED should preserve GS as 3.
	 */
	asm volatile (
		"mov %[rpl3], %%gs\n\t"
		"int3\n\t"
		"mov %%gs, %[res]"
		: [res] "=r" (gs_val)
		: [rpl3] "r" (3)
	);

	clearhandler(SIGTRAP);

	return gs_val == 3;
}

#endif /* __SELFTESTS_X86_HELPERS_H */
