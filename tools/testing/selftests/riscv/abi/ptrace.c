// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <linux/elf.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <asm/ptrace.h>

#include "../../kselftest_harness.h"

#define ORIG_A0_MODIFY      0x01
#define A0_MODIFY           0x02
#define A0_OLD              0x03
#define A0_NEW              0x04

struct a0_regs {
	__s64 orig_a0;
	__u64 a0;
};

#define perr_and_exit(fmt, ...)						\
	({								\
		char buf[256];						\
		snprintf(buf, sizeof(buf), "%s:%d:" fmt ": %m\n",	\
			__func__, __LINE__, ##__VA_ARGS__);		\
		ksft_exit_fail_perror(buf);				\
	})

static inline void resume_and_wait_tracee(pid_t pid, int flag)
{
	int status;

	if (ptrace(flag, pid, 0, 0))
		perr_and_exit("failed to resume the tracee %d\n", pid);

	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee %d\n", pid);
}

static void ptrace_test(int opt, struct a0_regs *result)
{
	int status;
	pid_t pid;
	struct user_regs_struct regs;
	struct iovec iov = {
		.iov_base = &regs,
		.iov_len = sizeof(regs),
	};

	unsigned long orig_a0;
	struct iovec a0_iov = {
		.iov_base = &orig_a0,
		.iov_len = sizeof(orig_a0),
	};
	struct ptrace_syscall_info syscall_info_entry, syscall_info_exit;

	pid = fork();
	if (pid == 0) {
		/* Mark oneself being traced */
		long val = ptrace(PTRACE_TRACEME, 0, 0, 0);

		if (val)
			perr_and_exit("failed to request for tracer to trace me: %ld\n", val);

		kill(getpid(), SIGSTOP);

		/* Perform exit syscall that will be intercepted */
		exit(A0_OLD);
	}

	if (pid < 0)
		ksft_exit_fail_perror("failed to fork");

	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee %d\n", pid);

	/* Stop at the entry point of the syscall */
	resume_and_wait_tracee(pid, PTRACE_SYSCALL);

	/* Check tracee regs before the syscall */
	if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov))
		perr_and_exit("failed to get tracee registers\n");
	if (ptrace(PTRACE_GETREGSET, pid, NT_RISCV_ORIG_A0, &a0_iov))
		perr_and_exit("failed to get tracee registers\n");
	if (orig_a0 != A0_OLD)
		perr_and_exit("unexpected orig_a0: 0x%lx\n", orig_a0);

	/* Modify a0/orig_a0 for the syscall */
	switch (opt) {
	case A0_MODIFY:
		regs.a0 = A0_NEW;
		break;
	case ORIG_A0_MODIFY:
		orig_a0 = A0_NEW;
		break;
	}

	if (ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov))
		perr_and_exit("failed to set tracee registers\n");
	if (ptrace(PTRACE_SETREGSET, pid, NT_RISCV_ORIG_A0, &a0_iov))
		perr_and_exit("failed to set tracee registers\n");

	if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, PTRACE_SYSCALL_INFO_ENTRY, &syscall_info_entry))
		perr_and_exit("failed to get syscall info of entry\n");
	result->orig_a0 = syscall_info_entry->entry.args[0];
	if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, PTRACE_SYSCALL_INFO_EXIT, &syscall_info_exit))
		perr_and_exit("failed to get syscall info of exit\n");
	result->a0 = syscall_info_exit->exit.rval;

	/* Resume the tracee */
	ptrace(PTRACE_CONT, pid, 0, 0);
	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee\n");

}

TEST(ptrace_modify_a0)
{
	struct a0_regs result;

	ptrace_test(A0_MODIFY, &result);

	/* The modification of a0 cannot affect the first argument of the syscall */
	EXPECT_EQ(A0_OLD, result.orig_a0);
	EXPECT_EQ(A0_NEW, result.a0);
}

TEST(ptrace_modify_orig_a0)
{
	struct a0_regs result;

	ptrace_test(ORIG_A0_MODIFY, &result);

	/* Only modify orig_a0 to change the first argument of the syscall */
	EXPECT_EQ(A0_NEW, result.orig_a0);
	/* a0 will keep default value, orig_a0 or -ENOSYS, depends on internal. */
	EXPECT_NE(A0_NEW, result.a0);
}

TEST_HARNESS_MAIN
