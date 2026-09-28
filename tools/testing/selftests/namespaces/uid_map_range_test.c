// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../kselftest_harness.h"

/*
 * The fields of a uid_map/gid_map line are stored in u32. Values that
 * do not fit must be rejected with EINVAL instead of being silently
 * truncated modulo 2^32, and in-range values must still be accepted.
 */

static int write_file(const char *path, const char *buf)
{
	ssize_t len = strlen(buf);
	ssize_t ret;
	int fd;

	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ret = write(fd, buf, len);
	if (ret < 0)
		ret = -errno;
	close(fd);
	if (ret < 0)
		return ret;
	return ret == len ? 0 : -EIO;
}

static ssize_t read_file(const char *path, char *buf, size_t size)
{
	ssize_t ret;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ret = read(fd, buf, size - 1);
	if (ret < 0)
		ret = -errno;
	else
		buf[ret] = '\0';
	close(fd);
	return ret;
}

/* Enter a fresh user namespace, remembering the outer ids. */
static int enter_userns(uid_t *uid, gid_t *gid)
{
	*uid = geteuid();
	*gid = getegid();
	if (unshare(CLONE_NEWUSER) < 0)
		return -errno;
	/* Required before an unprivileged gid_map write. */
	return write_file("/proc/self/setgroups", "deny");
}

static void expect_rejected(struct __test_metadata *_metadata,
			    const char *path, const char *line)
{
	char buf[256];

	TH_LOG("writing \"%s\" to %s", line, path);
	EXPECT_EQ(write_file(path, line), -EINVAL);
	/* Nothing may have been installed. */
	EXPECT_EQ(read_file(path, buf, sizeof(buf)), 0) {
		TH_LOG("installed: %s", buf);
	}
}

TEST(uid_map_first_above_u32)
{
	char line[64];
	uid_t uid;
	gid_t gid;
	int ret;

	ret = enter_userns(&uid, &gid);
	if (ret)
		SKIP(return, "cannot set up user namespace: %s", strerror(-ret));
	snprintf(line, sizeof(line), "4294967301 %u 1", uid);
	expect_rejected(_metadata, "/proc/self/uid_map", line);
}

TEST(uid_map_lower_first_above_u32)
{
	char line[64];
	uid_t uid;
	gid_t gid;
	int ret;

	ret = enter_userns(&uid, &gid);
	if (ret)
		SKIP(return, "cannot set up user namespace: %s", strerror(-ret));
	snprintf(line, sizeof(line), "0 %llu 1", (1ULL << 32) + uid);
	expect_rejected(_metadata, "/proc/self/uid_map", line);
}

TEST(uid_map_count_above_u32)
{
	char line[64];
	uid_t uid;
	gid_t gid;
	int ret;

	ret = enter_userns(&uid, &gid);
	if (ret)
		SKIP(return, "cannot set up user namespace: %s", strerror(-ret));
	snprintf(line, sizeof(line), "0 %u 4294967297", uid);
	expect_rejected(_metadata, "/proc/self/uid_map", line);
}

TEST(gid_map_first_above_u32)
{
	char line[64];
	uid_t uid;
	gid_t gid;
	int ret;

	ret = enter_userns(&uid, &gid);
	if (ret)
		SKIP(return, "cannot set up user namespace: %s", strerror(-ret));
	snprintf(line, sizeof(line), "4294967301 %u 1", gid);
	expect_rejected(_metadata, "/proc/self/gid_map", line);
}

TEST(uid_map_in_range)
{
	unsigned int first, lower_first, count;
	char line[64], buf[256];
	uid_t uid;
	gid_t gid;
	int ret;

	ret = enter_userns(&uid, &gid);
	if (ret)
		SKIP(return, "cannot set up user namespace: %s", strerror(-ret));
	snprintf(line, sizeof(line), "5 %u 1", uid);
	ASSERT_EQ(write_file("/proc/self/uid_map", line), 0);

	ASSERT_GT(read_file("/proc/self/uid_map", buf, sizeof(buf)), 0);
	ASSERT_EQ(sscanf(buf, "%u %u %u", &first, &lower_first, &count), 3);
	EXPECT_EQ(first, 5U);
	EXPECT_EQ(lower_first, uid);
	EXPECT_EQ(count, 1U);
	/* Exactly one extent. */
	EXPECT_EQ(strchr(buf, '\n'), buf + strlen(buf) - 1);
}

TEST_HARNESS_MAIN
