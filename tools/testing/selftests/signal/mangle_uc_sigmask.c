// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 ARM Ltd.
 *
 * Author: Dev Jain <dev.jain@arm.com>
 *
 * Test describing a clear distinction between signal states - delivered and
 * blocked, and their relation with ucontext.
 */

#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <ucontext.h>
#include <assert.h>

#include "../kselftest.h"

void handler_verify_ucontext(int signo, siginfo_t *info, void *uc)
{
	int ret;

	/* Kernel dumps ucontext with USR2 blocked */
	ret = sigismember(&(((ucontext_t *)uc)->uc_sigmask), SIGUSR2);
	ksft_test_result(ret == 1, "USR2 in ucontext\n");

	raise(SIGUSR2);
}

void handler_segv(int signo, siginfo_t *info, void *uc)
{
	/*
	 * Three cases possible:
	 * 1. Program already terminated due to segmentation fault.
	 * 2. SEGV was blocked even after returning from handler_usr.
	 * 3. SEGV was delivered on returning from handler_usr.
	 * The last option must happen.
	 */
	ksft_test_result_pass("SEGV delivered\n");
}

static int cnt;

void handler_usr(int signo, siginfo_t *info, void *uc)
{
	int ret;

	/*
	 * Break out of infinite recursion caused by raise(SIGUSR1) invoked
	 * from inside the handler
	 */
	++cnt;
	if (cnt > 1)
		return;

	ksft_print_msg("In handler_usr\n");

	/* SEGV blocked during handler execution, delivered on return */
	raise(SIGPIPE);
	ksft_print_msg("SEGV bypassed successfully\n");

	/*
	 * Signal responsible for handler invocation is blocked by default;
	 * delivered on return, leading to an infinite recursion
	 */
	raise(SIGUSR1);
	ksft_test_result(cnt == 1,
			 "USR1 is blocked, cannot invoke handler again\n");

	/* SIGPIPE has been blocked in sa_mask, but ucontext is invariant */
	ret = sigismember(&(((ucontext_t *)uc)->uc_sigmask), SIGPIPE);
	ksft_test_result(ret == 0, "USR1 not in ucontext\n");

	/* SIGUSR1 has been blocked, but ucontext is invariant */
	ret = sigismember(&(((ucontext_t *)uc)->uc_sigmask), SIGUSR1);
	ksft_test_result(ret == 0, "SEGV not in ucontext\n");

	/*
	 * Mangle ucontext; this will be copied back into &current->blocked
	 * on return from the handler.
	 */
	if (sigaddset(&((ucontext_t *)uc)->uc_sigmask, SIGUSR2))
		ksft_exit_fail_perror("Cannot add into uc_sigmask");
}

int main(int argc, char *argv[])
{
	struct sigaction act, act2;
	sigset_t *set, *oldset;

	ksft_print_header();
	ksft_set_plan(6);

	act.sa_flags = SA_SIGINFO;
	act.sa_sigaction = &handler_usr;

	/* add SEGV to blocked mask */
	if (sigemptyset(&act.sa_mask) || sigaddset(&act.sa_mask, SIGPIPE)
	    || (sigismember(&act.sa_mask, SIGPIPE) != 1))
		ksft_exit_fail_msg("Cannot add SEGV to blocked mask\n");

	if (sigaction(SIGUSR1, &act, NULL))
		ksft_exit_fail_perror("Cannot install handler");

	act2.sa_flags = SA_SIGINFO;
	act2.sa_sigaction = &handler_segv;

	if (sigaction(SIGPIPE, &act2, NULL))
		ksft_exit_fail_perror("Cannot install handler");

	/* invoke handler */
	raise(SIGUSR1);

	/* Mangled ucontext implies USR2 is blocked for current thread */
	raise(SIGUSR2);
	ksft_print_msg("USR2 bypassed successfully\n");

	act.sa_sigaction = &handler_verify_ucontext;
	if (sigaction(SIGUSR1, &act, NULL))
		ksft_exit_fail_perror("Cannot install handler");

	raise(SIGUSR1);

	ksft_print_msg("USR2 still blocked on return from handler\n");

	/* Confirm USR2 blockage by sigprocmask() too */
	set = malloc(sizeof(sigset_t *));
	oldset = malloc(sizeof(sigset_t *));

	if (sigemptyset(set))
		ksft_exit_fail_perror("Cannot empty set");

	if (sigprocmask(SIG_BLOCK, set, oldset))
		ksft_exit_fail_perror("sigprocmask()");

	ksft_test_result(sigismember(oldset, SIGUSR2) == 1,
			 "USR2 present in &current->blocked\n");

	ksft_finished();
}
