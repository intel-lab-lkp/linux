// SPDX-License-Identifier: GPL-2.0-only
/*
 * fsgsbase_restore.c, test ptrace vs fsgsbase
 * Copyright (c) 2020 Andy Lutomirski
 *
 * This test case simulates a tracer redirecting tracee execution to
 * a function and then restoring tracee state using PTRACE_GETREGS and
 * PTRACE_SETREGS.  This is similar to what gdb does when doing
 * 'p func()'.  The catch is that this test has the called function
 * modify a segment register.  This makes sure that ptrace correctly
 * restores segment state when using PTRACE_SETREGS.
 *
 * This is not part of fsgsbase.c, because that test is 64-bit only.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <err.h>
#include <sys/user.h>
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <asm/ldt.h>
#include <sys/mman.h>
#include <stddef.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <stdint.h>
#include "../kselftest.h"

#define EXPECTED_VALUE 0x1337f00d

#ifdef __x86_64__
# define SEG "%gs"
#else
# define SEG "%fs"
#endif

/*
 * Defined in clang_helpers_[32|64].S, because unlike gcc, clang inline asm does
 * not support segmentation prefixes.
 */
unsigned int dereference_seg_base(void);

static int init_seg(void)
{
	unsigned int *target = mmap(
		NULL, sizeof(unsigned int),
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (target == MAP_FAILED)
		ksft_exit_fail_perror("mmap");

	*target = EXPECTED_VALUE;

	ksft_print_msg("segment base address = 0x%lx\n", (unsigned long)target);

	struct user_desc desc = {
		.entry_number    = 0,
		.base_addr       = (unsigned int)(uintptr_t)target,
		.limit           = sizeof(unsigned int) - 1,
		.seg_32bit       = 1,
		.contents        = 0, /* Data, grow-up */
		.read_exec_only  = 0,
		.limit_in_pages  = 0,
		.seg_not_present = 0,
		.useable         = 0
	};
	if (syscall(SYS_modify_ldt, 1, &desc, sizeof(desc)) == 0) {
		ksft_print_msg("using LDT slot 0\n");
		asm volatile ("mov %0, %" SEG :: "rm" ((unsigned short)0x7));
	} else {
		/* No modify_ldt for us (configured out, perhaps) */

		struct user_desc *low_desc = mmap(
			NULL, sizeof(desc),
			PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		memcpy(low_desc, &desc, sizeof(desc));

		low_desc->entry_number = -1;

		/* 32-bit set_thread_area */
		long ret;
		asm volatile ("int $0x80"
			      : "=a" (ret), "+m" (*low_desc)
			      : "a" (243), "b" (low_desc)
#ifdef __x86_64__
			      : "r8", "r9", "r10", "r11"
#endif
			);
		memcpy(&desc, low_desc, sizeof(desc));
		munmap(low_desc, sizeof(desc));

		if (ret != 0) {
			ksft_print_msg("could not create a segment -- can't test anything\n");
			return KSFT_SKIP;
		}
		ksft_print_msg("using GDT slot %d\n", desc.entry_number);

		unsigned short sel = (unsigned short)((desc.entry_number << 3) | 0x3);
		asm volatile ("mov %0, %" SEG :: "rm" (sel));
	}

	return 0;
}

static void tracee_zap_segment(void)
{
	/*
	 * The tracer will redirect execution here.  This is meant to
	 * work like gdb's 'p func()' feature.  The tricky bit is that
	 * we modify a segment register in order to make sure that ptrace
	 * can correctly restore segment registers.
	 */
	ksft_print_msg("Tracee: in tracee_zap_segment()\n");

	/*
	 * Write a nonzero selector with base zero to the segment register.
	 * Using a null selector would defeat the test on AMD pre-Zen2
	 * CPUs, as such CPUs don't clear the base when loading a null
	 * selector.
	 */
	unsigned short sel;
	asm volatile ("mov %%ss, %0\n\t"
		      "mov %0, %" SEG
		      : "=rm" (sel));

	pid_t pid = getpid(), tid = syscall(SYS_gettid);

	ksft_print_msg("Tracee is going back to sleep\n");
	syscall(SYS_tgkill, pid, tid, SIGSTOP);

	/* Should not get here. */
	ksft_exit_fail_msg("Tracee hit unreachable code\n");
}

int main()
{
	int ret;

	ksft_print_header();
	ksft_set_plan(2);

	ksft_print_msg("Setting up a segment\n");

	ret = init_seg();
	if (ret)
		return ret;

	unsigned int val = dereference_seg_base();
	ksft_test_result(val == EXPECTED_VALUE, "The segment points to the right place.\n");

	pid_t chld = fork();
	if (chld < 0)
		ksft_exit_fail_perror("fork");

	if (chld == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0, 0);

		if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
			ksft_exit_fail_perror("PTRACE_TRACEME");

		pid_t pid = getpid(), tid = syscall(SYS_gettid);

		ksft_print_msg("Tracee will take a nap until signaled\n");
		syscall(SYS_tgkill, pid, tid, SIGSTOP);

		ksft_print_msg("Tracee was resumed. Will re-check segment.\n");

		val = dereference_seg_base();

		if (val == EXPECTED_VALUE) {
			ksft_print_msg("The segment points to the right place.\n");
			return EXIT_SUCCESS;
		}

		ksft_print_msg("seg[0] == %x; should be %x\n", val, EXPECTED_VALUE);
		return EXIT_FAILURE;
	}

	int status;

	/* Wait for SIGSTOP. */
	if (waitpid(chld, &status, 0) != chld || !WIFSTOPPED(status))
		ksft_exit_fail_perror("waitpid");

	struct user_regs_struct regs;

	if (ptrace(PTRACE_GETREGS, chld, NULL, &regs) != 0)
		ksft_exit_fail_perror("PTRACE_GETREGS");

#ifdef __x86_64__
	ksft_print_msg("Child GS=0x%lx, GSBASE=0x%lx\n", (unsigned long)regs.gs, (unsigned long)regs.gs_base);
#else
	ksft_print_msg("Child FS=0x%lx\n", (unsigned long)regs.xfs);
#endif

	struct user_regs_struct regs2 = regs;
#ifdef __x86_64__
	regs2.rip = (unsigned long)tracee_zap_segment;
	regs2.rsp -= 128;	/* Don't clobber the redzone. */
#else
	regs2.eip = (unsigned long)tracee_zap_segment;
#endif

	ksft_print_msg("Tracer: redirecting tracee to tracee_zap_segment()\n");
	if (ptrace(PTRACE_SETREGS, chld, NULL, &regs2) != 0)
		ksft_exit_fail_perror("PTRACE_GETREGS");
	if (ptrace(PTRACE_CONT, chld, NULL, NULL) != 0)
		ksft_exit_fail_perror("PTRACE_GETREGS");

	/* Wait for SIGSTOP. */
	if (waitpid(chld, &status, 0) != chld || !WIFSTOPPED(status))
		ksft_exit_fail_perror("waitpid");

	ksft_print_msg("Tracer: restoring tracee state\n");
	if (ptrace(PTRACE_SETREGS, chld, NULL, &regs) != 0)
		ksft_exit_fail_perror("PTRACE_GETREGS");
	if (ptrace(PTRACE_DETACH, chld, NULL, NULL) != 0)
		ksft_exit_fail_perror("PTRACE_GETREGS");

	/* Wait for SIGSTOP. */
	if (waitpid(chld, &status, 0) != chld)
		ksft_exit_fail_perror("waitpid");

	if (WIFSIGNALED(status))
		ksft_test_result_fail("Tracee crashed\n");
	else if (!WIFEXITED(status))
		ksft_test_result_fail("Tracee stopped for an unexpected reason: %d\n", status);
	else if (WEXITSTATUS(status) != 0)
		ksft_test_result_fail("Tracee reported failure\n");
	else
		ksft_test_result_pass("Tracee exited correctly\n");

	ksft_finished();
}
