// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#define __SANE_USERSPACE_TYPES__

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>

#include "kselftest_harness.h"
#include "wrappers.h"

TEST(kernfs_listxattr)
{
	char *buf, *xattr;
	ssize_t len, ret;
	int fd;

	/* Read-only file that can never have any extended attributes set.
	 * However, SELinux may set security.selinux xattr on kernfs files
	 * during policy load, so we explicitly ignore it.
	 */
	fd = open("/sys/kernel/warn_count", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);

	len = flistxattr(fd, NULL, 0);
	ASSERT_GE(len, 0);

	if (len > 0) {
		buf = malloc(len);
		ASSERT_NE(buf, NULL);

		ret = flistxattr(fd, buf, len);
		ASSERT_EQ(ret, len);

		for (xattr = buf; xattr < buf + len; xattr += strlen(xattr) + 1)
			ASSERT_EQ(strcmp(xattr, "security.selinux"), 0);

		free(buf);
	}

	EXPECT_EQ(close(fd), 0);
}

TEST(kernfs_getxattr)
{
	int fd;
	char buf[1];

	/* Read-only file that can never have any extended attributes set. */
	fd = open("/sys/kernel/warn_count", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_LT(fgetxattr(fd, "user.foo", buf, sizeof(buf)), 0);
	ASSERT_EQ(errno, ENODATA);
	EXPECT_EQ(close(fd), 0);
}

TEST_HARNESS_MAIN

