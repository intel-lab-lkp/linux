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
#include <linux/stddef.h>
#include <asm/ptrace.h>

#include "../../kselftest_harness.h"

#define ORIG_A0_MODIFY      0x01
#define A0_MODIFY           0x02
#define A0_OLD              0xbadbeefbeeff
#define A0_NEW              0xffeebfeebdab


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

static void ptrace_test(int opt, struct a0_regs result[])
{
	int status;
	long rc;
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
	struct ptrace_syscall_info syscall_info = {
		.op = 0xff,
	};
	const unsigned int expected_sci_entry_size =
		offsetofend(struct ptrace_syscall_info, entry.args);
	const unsigned int expected_sci_exit_size =
		offsetofend(struct ptrace_syscall_info, exit.is_error);

	pid = fork();
	if (pid == 0) {
		/* Mark oneself being traced */
		long val = ptrace(PTRACE_TRACEME, 0, 0, 0);

		if (val < 0)
			perr_and_exit("failed to request for tracer to trace me: %ld", val);

		kill(getpid(), SIGSTOP);

		/* Perform chdir syscall that will be intercepted */
		syscall(__NR_chdir, A0_OLD);

		exit(0);
	}

	if (pid < 0)
		ksft_exit_fail_perror("failed to fork");

	for (int i = 0; i < 3; i++) {
		if (waitpid(pid, &status, 0) != pid)
			perr_and_exit("failed to wait for the tracee %d", pid);
		if (WIFSTOPPED(status)) {
			switch (WSTOPSIG(status)) {
			case SIGSTOP:
				if (ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD) < 0)
					perr_and_exit("failed to set PTRACE_O_TRACESYSGOOD");
				break;
			case SIGTRAP|0x80:
				/* Modify twice so GET_SYSCALL_INFO get modified a0 and orig_a0 */
				if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov))
					perr_and_exit("failed to get tracee registers");
				if (ptrace(PTRACE_GETREGSET, pid, NT_RISCV_ORIG_A0, &a0_iov))
					perr_and_exit("failed to get tracee registers");

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
					perr_and_exit("failed to set tracee registers");
				if (ptrace(PTRACE_SETREGSET, pid, NT_RISCV_ORIG_A0, &a0_iov))
					perr_and_exit("failed to set tracee registers");
				switch (i) {
				case 1:
					/* Stop at the beginning of syscall */
					rc = ptrace(PTRACE_GET_SYSCALL_INFO, pid,
						sizeof(syscall_info), &syscall_info);
					if (rc < 0)
						perr_and_exit("failed to get syscall info of entry");
					if (rc < expected_sci_entry_size
						|| syscall_info.op != PTRACE_SYSCALL_INFO_ENTRY)
						perr_and_exit("stop position of entry mismatched");
					result[0].orig_a0 = syscall_info.entry.args[0];
					break;

				case 2:
					/* Stop at the end of syscall */
					rc = ptrace(PTRACE_GET_SYSCALL_INFO, pid,
						sizeof(syscall_info), &syscall_info);
					if (rc < 0)
						perr_and_exit("failed to get syscall info of entry");
					if (rc < expected_sci_exit_size
						|| syscall_info.op != PTRACE_SYSCALL_INFO_EXIT)
						perr_and_exit("stop position of exit mismatched");
					result[0].a0 = syscall_info.exit.rval;

					if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov))
						perr_and_exit("failed to get tracee registers");
					result[1].a0 = regs.a0;
					if (ptrace(PTRACE_GETREGSET, pid, NT_RISCV_ORIG_A0,
						   &a0_iov))
						perr_and_exit("failed to get tracee registers");
					result[1].orig_a0 = orig_a0;
				}
			}
			if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0)
				perr_and_exit("failed to resume tracee");
		}
	}

	/* Resume the tracee */
	ptrace(PTRACE_CONT, pid, 0, 0);
	if (waitpid(pid, &status, 0) != pid)
		perr_and_exit("failed to wait for the tracee");

}

TEST(ptrace_access_a0)
{
	struct a0_regs result[2];

	ptrace_test(A0_MODIFY, result);

	/* Verify PTRACE_SETREGSET */
	/* The modification of a0 cannot affect the first argument of the syscall */
	EXPECT_EQ(A0_OLD, result[0].orig_a0);
	EXPECT_EQ(A0_NEW, result[0].a0);

	/* Verify PTRACE_GETREGSET */
	EXPECT_EQ(result[1].orig_a0, result[0].orig_a0);
	EXPECT_EQ(result[1].a0, result[0].a0);
}

TEST(ptrace_access_orig_a0)
{
	struct a0_regs result[2];

	ptrace_test(ORIG_A0_MODIFY, result);

	/* Verify PTRACE_SETREGSET */
	/* Only modify orig_a0 to change the first argument of the syscall */
	EXPECT_EQ(A0_NEW, result[0].orig_a0);
	/* a0 will not be affected */
	EXPECT_NE(A0_NEW, result[0].a0);

	/* Verify PTRACE_GETREGSET */
	EXPECT_EQ(result[1].orig_a0, result[0].orig_a0);
	EXPECT_EQ(result[1].a0, result[0].a0);
}

TEST_HARNESS_MAIN
