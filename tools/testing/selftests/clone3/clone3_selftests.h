/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _CLONE3_SELFTESTS_H
#define _CLONE3_SELFTESTS_H

#define _GNU_SOURCE
#include <sched.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <stdint.h>
#include <syscall.h>
#include <sys/wait.h>

#include "kselftest.h"

#define ptr_to_u64(ptr) ((__u64)((uintptr_t)(ptr)))

#ifndef __NR_clone3
#define __NR_clone3 435
#endif

struct __clone_args {
	__aligned_u64 flags;
	__aligned_u64 pidfd;
	__aligned_u64 child_tid;
	__aligned_u64 parent_tid;
	__aligned_u64 exit_signal;
	__aligned_u64 stack;
	__aligned_u64 stack_size;
	__aligned_u64 tls;
	__aligned_u64 set_tid;
	__aligned_u64 set_tid_size;
	__aligned_u64 cgroup;
};

#if defined(__sparc__)
#include <errno.h>

#if	defined(__arch64__)
#define __SYSCALL_STRING						\
	"mov	%[scn], %%g1;"						\
	"ta	0x6d;"							\
	"bcc,pt	%%xcc, 1f;"						\
	" nop;"								\
	"sub	%%g0, %%o0, %%o0;"					\
	"1:"

#define __SYSCALL_CLOBBERS						\
	"g1", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",		\
	"f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",		\
	"f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",		\
	"f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",		\
	"f32", "f34", "f36", "f38", "f40", "f42", "f44", "f46",		\
	"f48", "f50", "f52", "f54", "f56", "f58", "f60", "f62",		\
	"cc", "memory"
#else
#define __SYSCALL_STRING						\
	"mov	%[scn], %%g1;"						\
	"ta	0x10;"							\
	"bcc	1f;"							\
	" nop;"								\
	"sub	%%g0, %%o0, %%o0;"					\
	"1:"

#define __SYSCALL_CLOBBERS						\
	"g1", "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",		\
	"f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",		\
	"f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",		\
	"f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31",		\
	"cc", "memory"
#endif

/* A special wrapper is required to handle the return call convention
 * on SPARC and is based on the INLINE_CLONE_SYSCALL macro in glibc.
 *
 * Parent -->  %o0 == child's  pid, %o1 == 0
 * Child  -->  %o0 == parent's pid, %o1 == 1
 */
static inline pid_t sparc_clone3(struct __clone_args *args, size_t size)
{
	long _cl_args = (long) (args);
	long _size = (long) (size);
	long _scn = __NR_clone3;

	register long o0 __asm__ ("o0") = _cl_args;
	register long o1 __asm__ ("o1") = _size;

	asm volatile (__SYSCALL_STRING
			: "=r" (o0), "=r" (o1)
			: [scn] "r" (_scn), "0" (o0), "1" (o1)
			: __SYSCALL_CLOBBERS);

	if ((unsigned long) (o0) > -4096UL) {
		errno = -o0;
		o0 = -1L;
	} else {
		o0 &= (o1 - 1);
	}

	return o0;
}

static pid_t sys_clone3(struct __clone_args *args, size_t size)
{
	fflush(stdout);
	fflush(stderr);
	return sparc_clone3(args, size);
}
#else
static pid_t sys_clone3(struct __clone_args *args, size_t size)
{
	fflush(stdout);
	fflush(stderr);
	return syscall(__NR_clone3, args, size);
}
#endif

static inline void test_clone3_supported(void)
{
	pid_t pid;
	struct __clone_args args = {};

	if (__NR_clone3 < 0)
		ksft_exit_skip("clone3() syscall is not supported\n");

	/* Set to something that will always cause EINVAL. */
	args.exit_signal = -1;
	pid = sys_clone3(&args, sizeof(args));
	if (!pid)
		exit(EXIT_SUCCESS);

	if (pid > 0) {
		wait(NULL);
		ksft_exit_fail_msg(
			"Managed to create child process with invalid exit_signal\n");
	}

	if (errno == ENOSYS)
		ksft_exit_skip("clone3() syscall is not supported\n");

	ksft_print_msg("clone3() syscall supported\n");
}

#endif /* _CLONE3_SELFTESTS_H */
