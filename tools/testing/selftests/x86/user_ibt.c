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
#include <stdint.h>
#include <sys/signal.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/const.h>
#include <linux/prctl.h>
#include <ucontext.h>
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
void __attribute__((naked, aligned(4096))) valid_target(void)
{
#ifdef __x86_64__
	asm volatile("endbr64\n");
#else
	asm volatile("endbr32\n");
#endif
	asm volatile(
		"ret\n"
		".p2align 12\n"
	);
}

void __attribute__((nocf_check, naked, aligned(4096))) invalid_target(void)
{
	asm volatile(
		"ret\n"
		".p2align 12\n"
	);
}

void __attribute__((naked)) user_ibt_basic_test(void)
{
#ifdef __x86_64__
	asm volatile(
		"leaq valid_target(%rip), %rax\n"
		"call *%rax\n"
		"ret\n"
	);
#else
	asm volatile(
		"movl $valid_target, %eax\n"
		"call *%eax\n"
		"ret\n"
	);
#endif
}

void __attribute__((naked)) user_ibt_notrack_test(void)
{
#ifdef __x86_64__
	asm volatile (
		"leaq invalid_target(%rip), %rax\n"
		"notrack call *%rax\n"
		"ret\n"
	);
#else
	asm volatile (
		"movl $invalid_target, %eax\n"
		"notrack call *%eax\n"
		"ret\n"
	);
#endif
}

static sigjmp_buf jmpbuf;
static volatile sig_atomic_t num_segv;
static volatile sig_atomic_t got_cperr;

static void segv_handler(int signum, siginfo_t *si, void *uc)
{
	num_segv++;
	got_cperr = si->si_code == SEGV_CPERR;
	siglongjmp(jmpbuf, 1);
}

int user_ibt_violation_test(void)
{
	struct sigaction sa = {};
	size_t volatile ptr;

	num_segv = 0;
	got_cperr = false;

	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &sa, NULL))
		return 0;

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

	signal(SIGSEGV, SIG_DFL);

	return num_segv == 1 && got_cperr;
}

static int user_ibt_signal_handler(void (*handler)(int, siginfo_t *, void *), bool expect_segv)
{
	struct sigaction sa = {};
	pid_t pid = getpid();

	if (pid < 0)
		return 0;
	num_segv = 0;
	got_cperr = false;

	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGPWR, &sa, NULL))
		return 0;

	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &sa, NULL))
		return 0;

	if (kill(getpid(), SIGPWR))
		return 0;

	signal(SIGSEGV, SIG_DFL);
	signal(SIGPWR, SIG_DFL);

	return 1;
}

int user_ibt_signal_handler_valid(void)
{
	user_ibt_signal_handler((void *)valid_target, false);
	return num_segv == 0;
}

bool has_endbr_preamble(void *ptr)
{
	const unsigned char *p = ptr;

	if (!(p[0] == 0xf3 && p[1] == 0x0f && p[2] == 0x1e))
		return false;
#ifdef __x86_64__
	return p[3] == 0xfa;
#else
	return p[3] == 0xfb;
#endif
}

int user_ibt_vdso(void)
{
	struct sigaction sa = {};
	struct timespec ts;
	int ret = 1;

	sa.sa_sigaction = segv_handler;
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &sa, NULL))
		return 0;

	if (!sigsetjmp(jmpbuf, 1))
		(void)clock_gettime(CLOCK_REALTIME, &ts);
	else
		ret = 0;

	signal(SIGSEGV, SIG_DFL);
	return ret;
}

int main(int argc, char *argv[])
{
	unsigned long lpad_status = PR_CFI_ENABLE;

	ksft_print_header();

	if (!has_endbr_preamble((void *)printf))
		ksft_exit_skip("libc does not support IBT (needs -fcf-protection=branch)\n");

	if (syscall(__NR_prctl, PR_SET_CFI, PR_CFI_BRANCH_LANDING_PADS, lpad_status, 0, 0)) {
		if (errno == EINVAL || errno == EOPNOTSUPP)
			ksft_exit_skip("User IBT is not supported.\n");
		ksft_exit_fail_perror("Failed to enable user IBT");
	}

	ksft_set_plan(5);

	user_ibt_basic_test();
	ksft_test_result_pass("valid indirect call with endbr64\n");

	user_ibt_notrack_test();
	ksft_test_result_pass("notrack indirect call to non-endbr target\n");

	ksft_test_result(user_ibt_violation_test(),
			 "indirect call to non-endbr target raises SIGSEGV\n");

	ksft_test_result(user_ibt_signal_handler_valid(),
			 "Signal handler sanity check\n");

	ksft_test_result(user_ibt_vdso(), "vDSO supports IBT\n");

	ksft_finished();
}
#endif