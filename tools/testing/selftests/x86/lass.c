// SPDX-License-Identifier: GPL-2.0
/*
 * Test Linear Address Space Separation (LASS) enforcement
 *
 * With LASS enabled, a user-mode read, write or instruction fetch at a kernel
 * address raises a #GP instead of the #PF that paging alone would produce.
 */
#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ucontext.h>

#include "helpers.h"

#ifndef __x86_64__
# error This test is 64-bit only
#endif

/*
 * Canonicality enforcement generates the same #GP as LASS for non-canonical
 * addresses. So, use a canonical address to specifically attribute the fault
 * to LASS. Bits 63:47 are all set here, which is canonical with 4-level
 * paging as well as 5-level paging.
 */
#define KERNEL_ADDR	0xffff800000000000UL

static sigjmp_buf jmpbuf;

static volatile unsigned long fault_trapno, fault_err, fault_rip;

/* Handle SIGSEGV (#GP and #PF) */
static void fault_handler(int sig, siginfo_t *info, void *ctx_void)
{
	ucontext_t *ctx = (ucontext_t *)ctx_void;

	fault_trapno = ctx->uc_mcontext.gregs[REG_TRAPNO];
	fault_err = ctx->uc_mcontext.gregs[REG_ERR];
	fault_rip = ctx->uc_mcontext.gregs[REG_RIP];
	siglongjmp(jmpbuf, 1);
}

/* Check CPUID bit corresponding to X86_FEATURE_LASS */
static bool is_lass_supported(void)
{
	unsigned int eax, ebx, ecx, edx;

	__cpuid_count(0x7, 0x1, eax, ebx, ecx, edx);
	return eax & (1 << 6);
}

/* A LASS violation raises a #GP with a null error code. */
static bool is_lass_violation(void)
{
	return fault_trapno == 13 && !fault_err;
}

static void test_kernel_read(void)
{
	if (sigsetjmp(jmpbuf, 1) == 0) {
		*(volatile unsigned long *)KERNEL_ADDR;
		ksft_test_result_fail("The read did not fault\n");
		return;
	}

	ksft_test_result(is_lass_violation(),
			 "The read faulted with trap=%ld, error=0x%lx\n",
			 fault_trapno, fault_err);
}

static void test_kernel_write(void)
{
	if (sigsetjmp(jmpbuf, 1) == 0) {
		*(volatile unsigned long *)KERNEL_ADDR = 0x1a55;
		ksft_test_result_fail("The write did not fault\n");
		return;
	}

	ksft_test_result(is_lass_violation(),
			 "The write faulted with trap=%ld, error=0x%lx\n",
			 fault_trapno, fault_err);
}

/*
 * Branch to the kernel address by hand: a direct 'call rel32' cannot reach
 * that far, and an indirect call through a function pointer is free to be
 * elided, which would stop testing the fetch.
 *
 * A 'jmp' fetches the target without writing the stack and never returns, so
 * there is no call-clobbered register state to describe.
 */
static void do_fetch(unsigned long addr)
{
	asm volatile ("jmp *%[fn]" : : [fn] "r" (addr));
}

static void test_kernel_fetch(void)
{
	if (sigsetjmp(jmpbuf, 1) == 0) {
		do_fetch(KERNEL_ADDR);

		/*
		 * Control does not come back from the jump, so reaching here
		 * means execution went somewhere unknown. Don't try to run
		 * the rest of the tests.
		 */
		ksft_exit_fail_msg("The fetch did not fault\n");
	}

	/*
	 * Branch instructions do not check their target against LASS. The
	 * violation happens when the target address is used to fetch the next
	 * instruction, so the fault must be reported at the target rather
	 * than at the branch.
	 */
	if (fault_rip != KERNEL_ADDR) {
		ksft_test_result_fail("The fetch faulted at RIP 0x%lx instead of 0x%lx\n",
				      fault_rip, KERNEL_ADDR);
		return;
	}

	ksft_test_result(is_lass_violation(),
			 "The fetch faulted with trap=%ld, error=0x%lx\n",
			 fault_trapno, fault_err);
}

#define TOTAL_TESTS 3

int main(void)
{
	ksft_print_header();

	/*
	 * Only the cpuinfo flag truly reflects whether the kernel enabled
	 * LASS. In practice, the kernel always enables LASS when the CPU
	 * supports it, so check the CPUID bit instead.
	 *
	 * The test is expected to fail if LASS is disabled via the kernel
	 * command line.
	 */
	if (!is_lass_supported())
		ksft_exit_skip("LASS is not present\n");

	ksft_set_plan(TOTAL_TESTS);

	sethandler(SIGSEGV, fault_handler, 0);

	ksft_print_msg("Accessing the kernel address 0x%lx from userspace\n",
		       KERNEL_ADDR);
	test_kernel_read();
	test_kernel_write();
	test_kernel_fetch();

	clearhandler(SIGSEGV);

	ksft_finished();
}
