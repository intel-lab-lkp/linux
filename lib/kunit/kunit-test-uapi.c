// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit Userspace selftest.
 *
 * Copyright (C) 2025, Linutronix GmbH.
 * Author: Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 *
 * This is *userspace* code.
 */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "../../tools/testing/selftests/kselftest.h"

static void test_procfs(void)
{
	char buf[256];
	ssize_t r;
	int fd;

	fd = open("/proc/self/comm", O_RDONLY);
	if (fd == -1) {
		ksft_test_result_fail("procfs: open() failed: %s\n", strerror(errno));
		return;
	}

	r = read(fd, buf, sizeof(buf));
	close(fd);

	if (r == -1) {
		ksft_test_result_fail("procfs: read() failed: %s\n", strerror(errno));
		return;
	}

	if (r != 16 || strncmp("kunit-test-uapi\n", buf, 16) != 0) {
		ksft_test_result_fail("procfs: incorrect comm\n");
		return;
	}

	ksft_test_result_pass("procfs\n");
}

int main(void)
{
	ksft_print_header();
	ksft_set_plan(1);
	test_procfs();
	ksft_finished();
}
