// SPDX-License-Identifier: GPL-2.0
/*
 * Linux Security Module infrastructure tests
 * Tests for the lockdown securityfs interface
 */

#define _GNU_SOURCE
#include <linux/lsm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include "kselftest_harness.h"
#include "common.h"

#define LOCKDOWN_PATH "/sys/kernel/security/lockdown"

static int write_status(const char *value)
{
	ssize_t len;
	int fd, err;

	fd = open(LOCKDOWN_PATH, O_WRONLY);
	if (fd < 0)
		return -1;

	len = write(fd, value, strlen(value));
	err = errno;
	close(fd);
	if (len < 0) {
		errno = err;
		return -1;
	}

	return 0;
}

static int status_writable(void)
{
	int fd;

	fd = open(LOCKDOWN_PATH, O_WRONLY);
	if (fd < 0)
		return -1;

	close(fd);
	return 0;
}

static int get_active_level(const char *buf, char *level, size_t size)
{
	const char *start, *end;
	size_t len;

	start = strchr(buf, '[');
	if (!start)
		return -1;
	end = strchr(start, ']');
	if (!end)
		return -1;

	len = end - start - 1;
	if (len == 0 || len >= size)
		return -1;

	memcpy(level, start + 1, len);
	level[len] = '\0';
	return 0;
}

static int read_lockdown_status(char *buf, size_t size)
{
	if (!read_file(LOCKDOWN_PATH, buf, size))
		return 0;

	return errno == ENOENT ? 1 : -1;
}

FIXTURE(lockdown)
{
	char status[128];
	char level[32];
};

FIXTURE_SETUP(lockdown)
{
	int ret;

	ret = read_lockdown_status(self->status, sizeof(self->status));
	if (ret) {
		if (ret > 0)
			SKIP(return, "%s is not present", LOCKDOWN_PATH);
		TH_LOG("cannot read %s: %s", LOCKDOWN_PATH, strerror(errno));
		ASSERT_EQ(0, ret);
	}
	if (geteuid() != 0)
		SKIP(return, "requires root");
	if (status_writable())
		SKIP(return, "cannot open %s for writing: %s",
		     LOCKDOWN_PATH, strerror(errno));

	ASSERT_EQ(0, get_active_level(self->status, self->level,
				      sizeof(self->level)));
}

FIXTURE_TEARDOWN(lockdown)
{
}

TEST_F(lockdown, read_format)
{
	ASSERT_NE(NULL, strstr(self->status, "none"));
	ASSERT_NE(NULL, strstr(self->status, "integrity"));
	ASSERT_NE(NULL, strstr(self->status, "confidentiality"));

	ASSERT_TRUE(!strcmp(self->level, "none") ||
		    !strcmp(self->level, "integrity") ||
		    !strcmp(self->level, "confidentiality"));
}

TEST_F(lockdown, invalid_write)
{
	char after[128];

	errno = 0;
	ASSERT_EQ(-1, write_status("invalid-level"));
	ASSERT_EQ(EINVAL, errno);

	ASSERT_EQ(0, read_file(LOCKDOWN_PATH, after, sizeof(after)));
	ASSERT_STREQ(self->status, after);
}

/*
 * This test raises the lockdown level and cannot undo it, so it runs
 * before the tests that expect an already locked down kernel.
 */
TEST_F(lockdown, upgrade)
{
	const char *next;
	char after[128], level[32];

	if (!strcmp(self->level, "none"))
		next = "integrity";
	else if (!strcmp(self->level, "integrity"))
		next = "confidentiality";
	else
		SKIP(return, "kernel is already at the highest level");

	errno = 0;
	ASSERT_EQ(0, write_status(next));

	ASSERT_EQ(0, read_file(LOCKDOWN_PATH, after, sizeof(after)));
	ASSERT_EQ(0, get_active_level(after, level, sizeof(level)));
	ASSERT_STREQ(next, level);
}

TEST_F(lockdown, same_level_rejected)
{
	char after[128];

	errno = 0;
	ASSERT_EQ(-1, write_status(self->level));
	ASSERT_EQ(EPERM, errno);

	ASSERT_EQ(0, read_file(LOCKDOWN_PATH, after, sizeof(after)));
	ASSERT_STREQ(self->status, after);
}

TEST_F(lockdown, no_downgrade)
{
	const char *lower;
	char after[128];

	if (!strcmp(self->level, "confidentiality"))
		lower = "integrity";
	else if (!strcmp(self->level, "integrity"))
		lower = "none";
	else
		SKIP(return, "kernel is not locked down, nothing to downgrade");

	errno = 0;
	ASSERT_EQ(-1, write_status(lower));
	ASSERT_EQ(EPERM, errno);

	ASSERT_EQ(0, read_file(LOCKDOWN_PATH, after, sizeof(after)));
	ASSERT_STREQ(self->status, after);
}

TEST_HARNESS_MAIN
