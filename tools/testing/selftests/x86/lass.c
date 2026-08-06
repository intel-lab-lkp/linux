// SPDX-License-Identifier: GPL-2.0
/*
 * lass.c - Test Linear Address Space Separation (LASS) enforcement
 *
 * With LASS enabled, a user-mode read, write or instruction fetch at a
 * kernel address raises a #GP instead of the #PF that SMAP/SMEP would
 * produce.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

#include "helpers.h"

#ifndef __x86_64__
# error This test is 64-bit only
#endif

/*
 * LASS rejects an address based on bit 63 alone, but a non-canonical
 * address raises the very same #GP for a different reason, so the
 * address has to be canonical to attribute the fault to LASS.
 *
 * Bits 63:47 are all set here, which is canonical with 4-level paging
 * as well as 5-level paging.
 */
#define KERNEL_ADDR	0xffff800000000000UL

static sigjmp_buf jmpbuf;

static volatile unsigned long fault_trapno, fault_err, fault_rip;

/* Handle SIGSEGV (#GP and #PF) as well as SIGBUS (#SS) */
static void fault_handler(int sig, siginfo_t *info, void *ctx_void)
{
	ucontext_t *ctx = (ucontext_t *)ctx_void;

	fault_trapno = ctx->uc_mcontext.gregs[REG_TRAPNO];
	fault_err = ctx->uc_mcontext.gregs[REG_ERR];
	fault_rip = ctx->uc_mcontext.gregs[REG_RIP];
	siglongjmp(jmpbuf, 1);
}

static bool is_lass_active(void)
{
	static const char delims[] = " \n";
	unsigned int eax, ebx, ecx, edx;
	bool found = false;
	char line[4096];
	FILE *cpuinfo;

	/*
	 * Only the cpuinfo flag reflects whether the kernel actually
	 * enabled LASS.
	 */
	cpuinfo = fopen("/proc/cpuinfo", "r");
	if (!cpuinfo)
		ksft_exit_fail_msg("failed to open /proc/cpuinfo\n");

	while (!found && fgets(line, sizeof(line), cpuinfo)) {
		char *flag;

		if (strncmp(line, "flags", 5))
			continue;

		/* Match whole words only, not a substring of another flag. */
		for (flag = strtok(line, delims); flag; flag = strtok(NULL, delims)) {
			if (!strcmp(flag, "lass")) {
				found = true;
				break;
			}
		}
	}

	fclose(cpuinfo);

	if (found)
		return true;

	/* Check CPUID.(EAX=07H,ECX=1):EAX.LASS[bit 6] */
	__cpuid_count(0x7, 0x1, eax, ebx, ecx, edx);
	if (eax & (1 << 6))
		ksft_print_msg("LASS is supported by the CPU but not enabled by the kernel\n");

	return false;
}

/* General Protection Fault (trapnr.h is not exported to uapi) */
#define X86_TRAP_GP	13

/* A LASS violation raises a #GP with a null error code. */
static bool is_lass_violation(void)
{
	return fault_trapno == X86_TRAP_GP && !fault_err;
}

static void test_kernel_read(void)
{
	if (sigsetjmp(jmpbuf, 1) == 0) {
		*(volatile unsigned long *)KERNEL_ADDR;
		ksft_test_result_fail("the read did not fault\n");
		return;
	}

	ksft_test_result(is_lass_violation(),
			 "the read faulted with trap=%ld, error=0x%lx\n",
			 fault_trapno, fault_err);
}

static void test_kernel_write(void)
{
	if (sigsetjmp(jmpbuf, 1) == 0) {
		*(volatile unsigned long *)KERNEL_ADDR = 0x1a55;
		ksft_test_result_fail("the write did not fault\n");
		return;
	}

	ksft_test_result(is_lass_violation(),
			 "the write faulted with trap=%ld, error=0x%lx\n",
			 fault_trapno, fault_err);
}

/*
 * Use inline asm rather than a call through a function pointer: a direct
 * 'call rel32' cannot reach a kernel address, and letting the compiler lower
 * the indirect branch risks routing it through a thunk, or eliding it
 * altogether, either of which would stop testing the fetch.
 */
static void do_fetch(unsigned long addr)
{
	asm volatile ("call *%[fn]"
		      : : [fn] "r" (addr)
		      : "memory", "cc", "rax", "rcx", "rdx", "rsi", "rdi",
			"r8", "r9", "r10", "r11");
}

static void test_kernel_fetch(void)
{
	if (sigsetjmp(jmpbuf, 1) == 0) {
		do_fetch(KERNEL_ADDR);

		/*
		 * Execution resumed at an unknown point with an undefined
		 * register state, so don't try to run the rest of the tests.
		 */
		ksft_exit_fail_msg("the fetch returned without faulting\n");
	}

	/*
	 * Branch instructions do not check their target against LASS. The
	 * violation happens when the target address is used to fetch the
	 * next instruction, so the fault must be reported at the target
	 * rather than at the branch.
	 */
	if (fault_rip != KERNEL_ADDR) {
		ksft_test_result_fail("the fetch faulted at RIP 0x%lx instead of 0x%lx\n",
				      fault_rip, (unsigned long)KERNEL_ADDR);
		return;
	}

	ksft_test_result(is_lass_violation(),
			 "the fetch faulted with trap=%ld, error=0x%lx\n",
			 fault_trapno, fault_err);
}

#define TOTAL_TESTS 3

int main(void)
{
	ksft_print_header();

	if (!is_lass_active())
		ksft_exit_skip("LASS is not enabled\n");

	ksft_set_plan(TOTAL_TESTS);

	sethandler(SIGSEGV, fault_handler, 0);
	/* Only to report a #SS; LASS shouldn't cause one here. */
	sethandler(SIGBUS, fault_handler, 0);

	ksft_print_msg("Accessing the kernel address 0x%lx from userspace\n",
		       (unsigned long)KERNEL_ADDR);
	test_kernel_read();
	test_kernel_write();
	test_kernel_fetch();

	clearhandler(SIGBUS);
	clearhandler(SIGSEGV);

	ksft_finished();
}
