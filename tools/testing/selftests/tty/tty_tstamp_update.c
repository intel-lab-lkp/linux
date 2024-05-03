// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/limits.h>

#include "../kselftest_harness.h"

#define MIN_TTY_PATH_LEN 8

static bool tty_valid(char *tty)
{
	if (strlen(tty) < MIN_TTY_PATH_LEN)
		return false;

	if (strncmp(tty, "/dev/tty", MIN_TTY_PATH_LEN) == 0 ||
	    strncmp(tty, "/dev/pts", MIN_TTY_PATH_LEN) == 0)
		return true;

	return false;
}

static int write_dev_tty(void)
{
	FILE *f;
	int r = 0;

	f = fopen("/dev/tty", "r+");
	if (!f)
		return -errno;

	r = fprintf(f, "hello, world!\n");
	if (r != strlen("hello, world!\n"))
		r = -EIO;

	fclose(f);
	return r;
}

TEST(tty_tstamp_update)
{
	int r;
	char tty[PATH_MAX] = {};
	struct stat st1, st2;

	ASSERT_GE(readlink("/proc/self/fd/0", tty, PATH_MAX), 0)
		TH_LOG("readlink on /proc/self/fd/0 failed: %m");

	ASSERT_TRUE(tty_valid(tty)) {
		TH_LOG("SKIP: invalid tty path '%s'", tty);
		_exit(KSFT_SKIP);
	}

	ASSERT_GE(stat(tty, &st1), 0)
		TH_LOG("stat failed on tty path '%s': %m", tty);

	/* We need to wait at least 8 seconds in order to observe timestamp change */
	/* https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=fbf47635315ab308c9b58a1ea0906e711a9228de */
	sleep(10);

	r = write_dev_tty();
	ASSERT_GE(r, 0)
		TH_LOG("failed to write to /dev/tty: %s", strerror(-r));

	ASSERT_GE(stat(tty, &st2), 0)
		TH_LOG("stat failed on tty path '%s': %m", tty);

	/* Validate unchanged atime under 'relatime' to ensure minimal disk I/O */
	EXPECT_EQ(st1.st_atim.tv_sec, st2.st_atim.tv_sec);

	/* We wrote to the terminal so timestamps should have been updated */
	ASSERT_NE(st1.st_mtim.tv_sec, st2.st_mtim.tv_sec)
		TH_LOG("tty timestamps not updated");

	TH_LOG("timestamps of terminal '%s' updated after write to /dev/tty",
	       tty);
}
TEST_HARNESS_MAIN
