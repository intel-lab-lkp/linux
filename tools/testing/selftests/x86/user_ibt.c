// SPDX-License-Identifier: GPL-2.0
/*
 * Test kernel support for userspace Indirect Branch Tracking (IBT).
 * Enables IBT manually via prctl(). Must be compiled with
 * -fcf-protection=branch to enable ENDBR64 instrumentation.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <linux/const.h>
#include <linux/prctl.h>
#include <unistd.h>
#include "../kselftest.h"

/*
 * Allow building this test with old kernel headers.
 */
#ifndef PR_SET_CFI
#define PR_GET_CFI			80
#define PR_SET_CFI			81
#endif
#ifndef PR_CFI_ENABLE
#define PR_CFI_ENABLE			(1UL << 0)
#define PR_CFI_DISABLE			(1UL << 1)
#endif
#ifndef PR_CFI_BRANCH_LANDING_PADS
#define PR_CFI_BRANCH_LANDING_PADS	0
#endif
#ifndef SEGV_CPERR
#define SEGV_CPERR			10
#endif

#if !defined(__CET__) || (__CET__ & 1) != 1
int main(int argc, char *argv[])
{
	ksft_print_header();
	ksft_exit_skip("Compiler does not support CET.\n");
	return 0;
}
#else
void __attribute__((naked)) valid_target(void)
{
	asm volatile (
		"endbr64\n"
		"ret\n"
	);
}

void __attribute__((nocf_check, naked)) invalid_target(void)
{
	asm volatile ("ret\n");
}

void __attribute__((naked)) user_ibt_basic_test(void)
{
	asm volatile (
		"leaq valid_target(%rip), %rax\n"
		"call *%rax\n"
		"ret\n"
	);
}

void __attribute__((naked)) user_ibt_notrack_test(void)
{
	asm volatile (
		"leaq invalid_target(%rip), %rax\n"
		"notrack call *%rax\n"
		"ret\n"
	);
}

static sigjmp_buf jmpbuf;
static sig_atomic_t got_signal;
static sig_atomic_t got_cperr;

static void segv_handler(int signum, siginfo_t *si, void *uc)
{
	got_signal = true;
	got_cperr = si->si_code == SEGV_CPERR;
	siglongjmp(jmpbuf, 1);
}

int user_ibt_violation_test(void)
{
	struct sigaction sa = {};
	struct sigaction oldsa;
	size_t volatile ptr;

	got_signal = false;
	got_cperr = false;

	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &sa, &oldsa)) {
		ksft_perror("SIGSEGV handler setup failed");
		return 1;
	}

	if (!sigsetjmp(jmpbuf, 1)) {
		/*
		 * Force an indirect call and drop 'nocf_check' attribute.
		 * Obfuscate cast through a temp local to suppress
		 * -Wincompatible-pointer-types (which correctly detects
		 * the nocf_check attribute mismatch)
		 */
		ptr = (size_t)invalid_target;
		((void (* volatile)(void))ptr)();
		/* Fall through in case this didn't SIGSEGV */
	}

	if (sigaction(SIGSEGV, &oldsa, NULL)) {
		ksft_perror("SIGSEGV handler restore failed");
		return 1;
	}

	if (!got_signal) {
		ksft_print_msg("IBT violation did not generate SIGSEGV\n");
		return 1;
	}
	if (!got_cperr) {
		ksft_print_msg("IBT violation generated unknown segfault\n");
		return 1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	unsigned long lpad_status = PR_CFI_ENABLE;

	ksft_print_header();
	ksft_set_plan(3);

	if (syscall(__NR_prctl, PR_SET_CFI, PR_CFI_BRANCH_LANDING_PADS, lpad_status, 0, 0)) {
		if (errno == EINVAL || errno == EOPNOTSUPP)
			ksft_exit_skip("User IBT is not supported.\n");
		ksft_exit_fail_perror("Failed to enable user IBT");
	}

	user_ibt_basic_test();
	ksft_test_result_pass("valid indirect call with endbr64\n");

	user_ibt_notrack_test();
	ksft_test_result_pass("notrack indirect call to non-endbr target\n");

	ksft_test_result(!user_ibt_violation_test(),
			 "indirect call to non-endbr target raises SIGSEGV\n");

	ksft_finished();
}
#endif
