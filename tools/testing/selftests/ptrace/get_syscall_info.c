// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2018 Dmitry V. Levin <ldv@altlinux.org>
 * All rights reserved.
 *
 * Check whether PTRACE_GET_SYSCALL_INFO semantics implemented in the kernel
 * matches userspace expectations.
 */

#include "../kselftest_harness.h"
#include <err.h>
#include <signal.h>
#include <asm/unistd.h>
#include <linux/types.h>
#include "linux/ptrace.h"

#if defined(__mips__) && defined(_ABIN32)
/*
 * MIPS N32 is the only architecture where __kernel_ulong_t
 * does not match the bitness of syscall arguments.
 */
typedef unsigned long long kernel_ulong_t;
#else
typedef __kernel_ulong_t kernel_ulong_t;
#endif

static int
kill_tracee(pid_t pid)
{
	if (!pid)
		return 0;

	int saved_errno = errno;

	int rc = kill(pid, SIGKILL);

	errno = saved_errno;
	return rc;
}

static long
sys_ptrace(int request, pid_t pid, unsigned long addr, unsigned long data)
{
	return syscall(__NR_ptrace, request, pid, addr, data);
}

#define LOG_KILL_TRACEE(fmt, ...)				\
	do {							\
		kill_tracee(pid);				\
		TH_LOG("wait #%d: " fmt,			\
		       ptrace_stop, ##__VA_ARGS__);		\
	} while (0)

TEST(get_syscall_info)
{
	const kernel_ulong_t args[][7] = {
		/* a sequence of architecture-agnostic syscalls */
		{
			__NR_chdir,
			(uintptr_t) "",
			(kernel_ulong_t) 0xdad1bef1bad1fed1ULL,
			(kernel_ulong_t) 0xdad2bef2bad2fed2ULL,
			(kernel_ulong_t) 0xdad3bef3bad3fed3ULL,
			(kernel_ulong_t) 0xdad4bef4bad4fed4ULL,
			(kernel_ulong_t) 0xdad5bef5bad5fed5ULL
		},
		{
			__NR_gettid,
			(kernel_ulong_t) 0xdad0bef0caf0bea0ULL,
			(kernel_ulong_t) 0xdad1bef1caf1bea1ULL,
			(kernel_ulong_t) 0xdad2bef2caf2bea2ULL,
			(kernel_ulong_t) 0xdad3bef3caf3bea3ULL,
			(kernel_ulong_t) 0xdad4bef4caf4bea4ULL,
			(kernel_ulong_t) 0xdad5bef5caf5bea5ULL
		},
		{
			__NR_exit_group,
			0,
			(kernel_ulong_t) 0xdad1bef1fac1c0d1ULL,
			(kernel_ulong_t) 0xdad2bef2fac2c0d2ULL,
			(kernel_ulong_t) 0xdad3bef3fac3c0d3ULL,
			(kernel_ulong_t) 0xdad4bef4fac4c0d4ULL,
			(kernel_ulong_t) 0xdad5bef5fac5c0d5ULL
		}
	};
	const kernel_ulong_t *exp_args;

	pid_t pid = fork();

	ASSERT_LE(0, pid) {
		TH_LOG("fork: %m");
	}

	if (pid == 0) {
		/* get the pid before PTRACE_TRACEME */
		pid = getpid();
		ASSERT_EQ(0, sys_ptrace(PTRACE_TRACEME, 0, 0, 0)) {
			TH_LOG("PTRACE_TRACEME: %m");
		}
		ASSERT_EQ(0, kill(pid, SIGSTOP)) {
			/* cannot happen */
			TH_LOG("kill SIGSTOP: %m");
		}
		for (unsigned int i = 0; i < ARRAY_SIZE(args); ++i) {
			syscall(args[i][0],
				args[i][1], args[i][2], args[i][3],
				args[i][4], args[i][5], args[i][6]);
		}
		/* unreachable */
		_exit(1);
	}

	const struct {
		unsigned int is_error;
		int rval;
	} *exp_param, exit_param[] = {
		{ 1, -ENOENT },	/* chdir */
		{ 0, pid }	/* gettid */
	};

	unsigned int ptrace_stop;

	for (ptrace_stop = 0; ; ++ptrace_stop) {
		struct ptrace_syscall_info info = {
			.op = 0xff	/* invalid PTRACE_SYSCALL_INFO_* op */
		};
		const size_t size = sizeof(info);
		const int expected_none_size =
			(void *) &info.entry - (void *) &info;
		const int expected_entry_size =
			(void *) &info.entry.args[6] - (void *) &info;
		const int expected_exit_size =
			(void *) (&info.exit.is_error + 1) -
			(void *) &info;
		int status;
		long rc;

		ASSERT_EQ(pid, wait(&status)) {
			/* cannot happen */
			LOG_KILL_TRACEE("wait: %m");
		}
		if (WIFEXITED(status)) {
			pid = 0;	/* the tracee is no more */
			ASSERT_EQ(0, WEXITSTATUS(status));
			break;
		}
		ASSERT_FALSE(WIFSIGNALED(status)) {
			pid = 0;	/* the tracee is no more */
			LOG_KILL_TRACEE("unexpected signal %u",
					WTERMSIG(status));
		}
		ASSERT_TRUE(WIFSTOPPED(status)) {
			/* cannot happen */
			LOG_KILL_TRACEE("unexpected wait status %#x", status);
		}

		switch (WSTOPSIG(status)) {
		case SIGSTOP:
			ASSERT_EQ(0, ptrace_stop) {
				LOG_KILL_TRACEE("unexpected signal stop");
			}
			ASSERT_EQ(0, sys_ptrace(PTRACE_SETOPTIONS, pid, 0,
						PTRACE_O_TRACESYSGOOD)) {
				LOG_KILL_TRACEE("PTRACE_SETOPTIONS: %m");
			}
			ASSERT_LT(0, (rc = sys_ptrace(PTRACE_GET_SYSCALL_INFO,
						      pid, size,
						      (uintptr_t) &info))) {
				LOG_KILL_TRACEE("PTRACE_GET_SYSCALL_INFO: %m");
			}
			ASSERT_EQ(expected_none_size, rc) {
				LOG_KILL_TRACEE("signal stop mismatch");
			}
			ASSERT_EQ(PTRACE_SYSCALL_INFO_NONE, info.op) {
				LOG_KILL_TRACEE("signal stop mismatch");
			}
			ASSERT_TRUE(info.arch) {
				LOG_KILL_TRACEE("signal stop mismatch");
			}
			ASSERT_TRUE(info.instruction_pointer) {
				LOG_KILL_TRACEE("signal stop mismatch");
			}
			ASSERT_TRUE(info.stack_pointer) {
				LOG_KILL_TRACEE("signal stop mismatch");
			}
			break;

		case SIGTRAP | 0x80:
			ASSERT_LT(0, (rc = sys_ptrace(PTRACE_GET_SYSCALL_INFO,
						      pid, size,
						      (uintptr_t) &info))) {
				LOG_KILL_TRACEE("PTRACE_GET_SYSCALL_INFO: %m");
			}
			switch (ptrace_stop) {
			case 1: /* entering chdir */
			case 3: /* entering gettid */
			case 5: /* entering exit_group */
				exp_args = args[ptrace_stop / 2];
				ASSERT_EQ(expected_entry_size, rc) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(PTRACE_SYSCALL_INFO_ENTRY, info.op) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_TRUE(info.arch) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_TRUE(info.instruction_pointer) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_TRUE(info.stack_pointer) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[0], info.entry.nr) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[1], info.entry.args[0]) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[2], info.entry.args[1]) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[3], info.entry.args[2]) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[4], info.entry.args[3]) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[5], info.entry.args[4]) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				ASSERT_EQ(exp_args[6], info.entry.args[5]) {
					LOG_KILL_TRACEE("entry stop mismatch");
				}
				break;
			case 2: /* exiting chdir */
			case 4: /* exiting gettid */
				exp_param = &exit_param[ptrace_stop / 2 - 1];
				ASSERT_EQ(expected_exit_size, rc) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				ASSERT_EQ(PTRACE_SYSCALL_INFO_EXIT, info.op) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				ASSERT_TRUE(info.arch) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				ASSERT_TRUE(info.instruction_pointer) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				ASSERT_TRUE(info.stack_pointer) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				ASSERT_EQ(exp_param->is_error,
					  info.exit.is_error) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				ASSERT_EQ(exp_param->rval, info.exit.rval) {
					LOG_KILL_TRACEE("exit stop mismatch");
				}
				break;
			default:
				LOG_KILL_TRACEE("unexpected syscall stop");
				abort();
			}
			break;

		default:
			LOG_KILL_TRACEE("unexpected stop signal %#x",
					WSTOPSIG(status));
			abort();
		}

		ASSERT_EQ(0, sys_ptrace(PTRACE_SYSCALL, pid, 0, 0)) {
			LOG_KILL_TRACEE("PTRACE_SYSCALL: %m");
		}
	}

	ASSERT_EQ(ARRAY_SIZE(args) * 2, ptrace_stop);
}

TEST_HARNESS_MAIN
