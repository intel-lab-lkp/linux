// SPDX-License-Identifier: GPL-2.0

#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <init.h>
#include "internal.h"

int host_has_fsgsbase;
/* those definitions can be pulled from os.h but if we include this
 * it shows conflicts of jmp_buf definitions in longjmp.h (UM) and
 * host one.  thus we declared here instead.
 */
void os_info(const char *fmt, ...);
void os_warn(const char *fmt, ...);

/**
 * get_host_cpu_features() return true with X86_FEATURE_FSGSBASE even
 * if the kernel is older and disabled using fsgsbase instruction.
 * thus detection is based on whether SIGILL is raised or not.
 */
static jmp_buf jmpbuf;
static void sigill(int sig, siginfo_t *si, void *ctx_void)
{
	siglongjmp(jmpbuf, 1);
}

void __init check_fsgsbase(void)
{
	unsigned long fsbase;
	struct sigaction sa;

	/* Probe FSGSBASE */
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = sigill;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGILL, &sa, 0))
		os_warn("sigaction");

	os_info("Checking FSGSBASE instructions...");
	if (sigsetjmp(jmpbuf, 0) == 0) {
		asm volatile("rdfsbase %0" : "=r" (fsbase) :: "memory");
		host_has_fsgsbase = 1;
		os_info("OK\n");
	} else {
		host_has_fsgsbase = 0;
		os_info("disabled\n");
	}
}
