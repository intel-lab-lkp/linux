// SPDX-License-Identifier: GPL-2.0-only
/*
 * Corrupt the XSTATE header in a signal frame
 *
 * Based on analysis and a test case from Thomas Gleixner.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <err.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>

#include "../kselftest.h" /* For __cpuid_count() */

static inline int xsave_enabled(void)
{
	unsigned int eax, ebx, ecx, edx;

	__cpuid_count(0x1, 0x0, eax, ebx, ecx, edx);

	/* Is CR4.OSXSAVE enabled ? */
	return ecx & (1U << 27);
}

static void sethandler(int sig, void (*handler)(int, siginfo_t *, void *),
		       int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO | flags;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, 0))
		ksft_exit_fail_perror("sigaction");
}

static void sigusr1(int sig, siginfo_t *info, void *uc_void)
{
	ucontext_t *uc = uc_void;
	uint8_t *fpstate = (uint8_t *)uc->uc_mcontext.fpregs;
	uint64_t *xfeatures = (uint64_t *)(fpstate + 512);

	ksft_print_msg("Wreck XSTATE header\n");
	/* Wreck the first reserved bytes in the header */
	*(xfeatures + 2) = 0xfffffff;
}

static void sigsegv(int sig, siginfo_t *info, void *uc_void)
{
	ksft_print_msg("Got SIGSEGV\n");
}

int main(void)
{
	cpu_set_t set;

	ksft_print_header();
	ksft_set_plan(2);

	sethandler(SIGUSR1, sigusr1, 0);
	sethandler(SIGSEGV, sigsegv, 0);

	if (!xsave_enabled()) {
		ksft_print_msg("CR4.OSXSAVE disabled.\n");
		return KSFT_SKIP;
	}

	CPU_ZERO(&set);
	CPU_SET(0, &set);

	/*
	 * Enforce that the child runs on the same CPU
	 * which in turn forces a schedule.
	 */
	sched_setaffinity(getpid(), sizeof(set), &set);

	ksft_print_msg("Send ourselves a signal\n");
	raise(SIGUSR1);

	ksft_test_result_pass("Back from the signal. Now schedule.\n");

	pid_t child = fork();
	if (child == 0)
		return 0;

	if (child < 0) {
		ksft_test_result_fail("fork: %s\n", strerror(errno));
	} else if (child) {
		waitpid(child, NULL, 0);
		ksft_test_result_pass("Back in the main thread.\n");
	}

	/*
	 * We could try to confirm that extended state is still preserved
	 * when we schedule.  For now, the only indication of failure is
	 * a warning in the kernel logs.
	 */

	ksft_finished();
}
