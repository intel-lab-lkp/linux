// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kernel.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <unistd.h>
#include <sys/resource.h>
#include <linux/close_range.h>

#include "../kselftest_harness.h"
#include "../clone3/clone3_selftests.h"

TEST(unshare_EMFILE)
{
	pid_t pid;
	int status;
	struct __clone_args args = {
		.flags = CLONE_FILES,
		.exit_signal = SIGCHLD,
	};
	int fd;
	ssize_t n, n2;
	static char buf[512], buf2[512];
	int nr_open;
	int bits_per_long = sizeof(long) * 8;

	fd = open("/proc/sys/fs/nr_open", O_RDWR);
	ASSERT_GE(fd, 0);

	n = read(fd, buf, sizeof(buf));
	ASSERT_GT(n, 0);
	ASSERT_EQ(buf[n - 1], '\n');

	ASSERT_EQ(sscanf(buf, "%d", &nr_open), 1);

	/* get a descriptor >= bits_per_long */
	EXPECT_GE(dup2(2, bits_per_long+1), 0) {
		lseek(fd, 0, SEEK_SET);
		write(fd, buf, n);
		exit(EXIT_FAILURE);
	}

	/* get descriptor table shared */
	pid = sys_clone3(&args, sizeof(args));
	EXPECT_GE(pid, 0) {
		lseek(fd, 0, SEEK_SET);
		write(fd, buf, n);
		exit(EXIT_FAILURE);
	}

	if (pid == 0) {
		int err;

		/* set nr_open == bits_per_long */
		n2 = sprintf(buf2, "%d\n", bits_per_long);
		lseek(fd, 0, SEEK_SET);
		write(fd, buf2, n2);

		/* ... and now unshare(CLONE_FILES) must fail with EMFILE */
		err = unshare(CLONE_FILES);
		EXPECT_EQ(err, -1)
			exit(EXIT_FAILURE);
		EXPECT_EQ(errno, EMFILE)
			exit(EXIT_FAILURE);

		/* restore fs.nr_open */
		lseek(fd, 0, SEEK_SET);
		write(fd, buf, n);
		exit(EXIT_SUCCESS);
	}

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_EQ(true, WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
}

TEST_HARNESS_MAIN
