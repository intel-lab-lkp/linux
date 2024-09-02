// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author: Aleksa Sarai <cyphar@cyphar.com>
 * Copyright (C) 2024 SUSE LLC
 */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>

#include "../kselftest.h"
#include "clone3_selftests.h"

#ifndef CHECK_FIELDS
#define CHECK_FIELDS (1ULL << 63)
#endif

#ifndef EEXTSYS_NOOP
#define EEXTSYS_NOOP 134
#endif

struct __clone_args_v0 {
	__aligned_u64 flags;
	__aligned_u64 pidfd;
	__aligned_u64 child_tid;
	__aligned_u64 parent_tid;
	__aligned_u64 exit_signal;
	__aligned_u64 stack;
	__aligned_u64 stack_size;
	__aligned_u64 tls;
};

struct __clone_args_v1 {
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
};

struct __clone_args_v2 {
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

static int call_clone3(void *clone_args, size_t size)
{
	int status;
	pid_t pid;

	pid = sys_clone3(clone_args, size);
	if (pid < 0) {
		ksft_print_msg("%d (%s) - Failed to create new process\n",
			       errno, strerror(errno));
		return -errno;
	}

	if (pid == 0) {
		ksft_print_msg("I am the child, my PID is %d\n", getpid());
		_exit(EXIT_SUCCESS);
	}

	ksft_print_msg("I am the parent (%d). My child's pid is %d\n",
			getpid(), pid);

	if (waitpid(-1, &status, __WALL) < 0) {
		ksft_print_msg("waitpid() returned %s\n", strerror(errno));
		return -errno;
	}
	if (!WIFEXITED(status)) {
		ksft_print_msg("Child did not exit normally, status 0x%x\n",
			       status);
		return EXIT_FAILURE;
	}
	if (WEXITSTATUS(status))
		return WEXITSTATUS(status);

	return 0;
}

static bool check(bool *failed, bool pred)
{
	*failed |= pred;
	return pred;
}

static void test_clone3_check_fields(const char *test_name, size_t struct_size)
{
	size_t bufsize;
	void *buffer;
	pid_t pid;
	bool failed = false;
	void (*resultfn)(const char *msg, ...) = ksft_test_result_pass;

	/* Allocate some bytes after clone_args to verify that the . */
	bufsize = struct_size + 16;
	buffer = malloc(bufsize);
	memset(buffer, 0, bufsize);

	pid = call_clone3(buffer, CHECK_FIELDS | struct_size);
	if (check(&failed, (pid != -EEXTSYS_NOOP)))
		ksft_print_msg("clone3(CHECK_FIELDS) returned the wrong error code: %d (%s)\n",
			       pid, strerror(-pid));

	switch (struct_size) {
	case sizeof(struct __clone_args_v2): {
		struct __clone_args_v2 *args = buffer;

		if (check(&failed, (args->cgroup != 0xFFFFFFFFFFFFFFFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong cgroup field: 0x%.16llx != 0x%.16llx\n",
				       args->cgroup, 0xFFFFFFFFFFFFFFFF);

		/* fallthrough; */
	}
	case sizeof(struct __clone_args_v1): {
		struct __clone_args_v1 *args = buffer;

		if (check(&failed, (args->set_tid != 0xFFFFFFFFFFFFFFFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong set_tid field: 0x%.16llx != 0x%.16llx\n",
				       args->set_tid, 0xFFFFFFFFFFFFFFFF);
		if (check(&failed, (args->set_tid_size != 0xFFFFFFFFFFFFFFFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong set_tid_size field: 0x%.16llx != 0x%.16llx\n",
				       args->set_tid_size, 0xFFFFFFFFFFFFFFFF);

		/* fallthrough; */
	}
	case sizeof(struct __clone_args_v0): {
		struct __clone_args_v0 *args = buffer;

		if (check(&failed, !(args->flags & CLONE_NEWUSER)))
			ksft_print_msg("clone3(CHECK_FIELDS) is missing CLONE_NEWUSER in flags: 0x%.16llx (0x%.16llx)\n",
				       args->flags, CLONE_NEWUSER);
		if (check(&failed, !(args->flags & CLONE_THREAD)))
			ksft_print_msg("clone3(CHECK_FIELDS) is missing CLONE_THREAD in flags: 0x%.16llx (0x%.16llx)\n",
				       args->flags, CLONE_THREAD);
		/*
		 * CLONE_INTO_CGROUP was added in v2, but it will be set even
		 * with smaller structure sizes.
		 */
		if (check(&failed, !(args->flags & CLONE_INTO_CGROUP)))
			ksft_print_msg("clone3(CHECK_FIELDS) is missing CLONE_INTO_CGROUP in flags: 0x%.16llx (0x%.16llx)\n",
				       args->flags, CLONE_INTO_CGROUP);

		if (check(&failed, (args->exit_signal != 0xFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong exit_signal field: 0x%.16llx != 0x%.16llx\n",
				       args->exit_signal, 0xFF);

		if (check(&failed, (args->stack != 0xFFFFFFFFFFFFFFFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong stack field: 0x%.16llx != 0x%.16llx\n",
				       args->stack, 0xFFFFFFFFFFFFFFFF);
		if (check(&failed, (args->stack_size != 0xFFFFFFFFFFFFFFFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong stack_size field: 0x%.16llx != 0x%.16llx\n",
				       args->stack_size, 0xFFFFFFFFFFFFFFFF);
		if (check(&failed, (args->tls != 0xFFFFFFFFFFFFFFFF)))
			ksft_print_msg("clone3(CHECK_FIELDS) has wrong tls field: 0x%.16llx != 0x%.16llx\n",
				       args->tls, 0xFFFFFFFFFFFFFFFF);

		break;
	}
	default:
		fprintf(stderr, "INVALID STRUCTURE SIZE: %d\n", struct_size);
		abort();
	}

	/* Verify that the trailing parts of the buffer are still 0. */
	for (size_t i = struct_size; i < bufsize; i++) {
		char ch = ((char *)buffer)[i];
		if (check(&failed, (ch != '\x00')))
			ksft_print_msg("clone3(CHECK_FIELDS) touched a byte outside the size: buffer[%d] = 0x%.2x\n",
				       i, ch);
	}

	if (failed)
		resultfn = ksft_test_result_fail;

	resultfn("clone3(CHECK_FIELDS) with %s\n", test_name);
	free(buffer);
}

struct check_fields_test {
	const char *name;
	size_t struct_size;
};

static struct check_fields_test check_fields_tests[] = {
	{"struct v0", sizeof(struct __clone_args_v0)},
	{"struct v1", sizeof(struct __clone_args_v1)},
	{"struct v2", sizeof(struct __clone_args_v2)},
};

int main(void)
{
	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(check_fields_tests));
	test_clone3_supported();

	for (int i = 0; i < ARRAY_SIZE(check_fields_tests); i++) {
		struct check_fields_test *test = &check_fields_tests[i];
		test_clone3_check_fields(test->name, test->struct_size);
	}

	ksft_finished();
}
