// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kselftest_harness.h"

FIXTURE(fileattr) {
	char workdir[PATH_MAX];
};

static int open_nofollow(const char *path)
{
	return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
}

static int get_xattr(int fd, struct fsxattr *fa)
{
	memset(fa, 0, sizeof(*fa));
	return ioctl(fd, FS_IOC_FSGETXATTR, fa);
}

static int set_xattr(int fd, struct fsxattr *fa)
{
	return ioctl(fd, FS_IOC_FSSETXATTR, fa);
}

static int build_path(char *buf, size_t size, const char *dir, const char *name)
{
	int ret;

	ret = snprintf(buf, size, "%s/%s", dir, name);
	if (ret < 0 || ret >= size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return 0;
}

static int make_workdir(char *workdir, size_t size)
{
	const char *base = getenv("FILEATTR_TEST_DIR");
	int ret;

	if (!base || !*base)
		base = P_tmpdir;

	if (base[0] != '/') {
		errno = EINVAL;
		return -1;
	}

	ret = snprintf(workdir, size, "%s/fileattr.XXXXXX", base);
	if (ret < 0 || ret >= (int)size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	return mkdtemp(workdir) ? 0 : -1;
}

static int cleanup_workdir(const char *workdir)
{
	struct dirent *de;
	DIR *dir;
	int ret;

	dir = opendir(workdir);
	if (!dir)
		return -1;

	while ((de = readdir(dir))) {
		struct stat st;
		char path[PATH_MAX];

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;

		ret = build_path(path, sizeof(path), workdir, de->d_name);
		if (ret) {
			closedir(dir);
			return -1;
		}

		ret = lstat(path, &st);
		if (ret) {
			closedir(dir);
			return -1;
		}

		if (S_ISDIR(st.st_mode))
			ret = rmdir(path);
		else
			ret = unlink(path);
		if (ret) {
			closedir(dir);
			return -1;
		}
	}

	closedir(dir);
	return rmdir(workdir);
}

FIXTURE_SETUP(fileattr)
{
	if (make_workdir(self->workdir, sizeof(self->workdir)))
		SKIP(return, "failed to create workdir: %s", strerror(errno));
}

FIXTURE_TEARDOWN(fileattr)
{
	EXPECT_EQ(cleanup_workdir(self->workdir), 0);
}

TEST_F(fileattr, get_regular_file)
{
	char path[PATH_MAX];
	struct fsxattr fa;
	int fd;

	ASSERT_EQ(build_path(path, sizeof(path), self->workdir, "regular.XXXXXX"), 0);

	fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);

	fd = open_nofollow(path);
	ASSERT_GE(fd, 0);

	ASSERT_EQ(get_xattr(fd, &fa), 0);

	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(unlink(path), 0);
}

TEST_F(fileattr, get_directory)
{
	char template[PATH_MAX];
	struct fsxattr fa;
	char *dir;
	int fd;

	ASSERT_EQ(build_path(template, sizeof(template), self->workdir, "dir.XXXXXX"), 0);

	dir = mkdtemp(template);
	ASSERT_NE(dir, NULL);

	fd = open_nofollow(dir);
	ASSERT_GE(fd, 0);

	ASSERT_EQ(get_xattr(fd, &fa), 0);

	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(rmdir(dir), 0);
}

TEST_F(fileattr, get_fifo)
{
	char path[PATH_MAX];
	struct fsxattr fa;
	int fd;

	ASSERT_EQ(build_path(path, sizeof(path), self->workdir, "fifo.XXXXXX"), 0);

	fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);
	ASSERT_EQ(unlink(path), 0);
	ASSERT_EQ(mkfifo(path, 0600), 0);

	fd = open_nofollow(path);
	ASSERT_GE(fd, 0);

	memset(&fa, 0, sizeof(fa));
	ASSERT_LT(ioctl(fd, FS_IOC_FSGETXATTR, &fa), 0);
	EXPECT_EQ(errno, ENOTTY);

	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(unlink(path), 0);
}

TEST_F(fileattr, set_nodump_roundtrip)
{
	char path[PATH_MAX];
	struct fsxattr fa, orig;
	int fd;

	ASSERT_EQ(build_path(path, sizeof(path), self->workdir, "nodump.XXXXXX"), 0);

	fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);

	fd = open_nofollow(path);
	ASSERT_GE(fd, 0);

	ASSERT_EQ(get_xattr(fd, &orig), 0);
	fa = orig;
	fa.fsx_xflags |= FS_XFLAG_NODUMP;
	ASSERT_EQ(set_xattr(fd, &fa), 0);
	ASSERT_EQ(get_xattr(fd, &fa), 0);
	EXPECT_TRUE(fa.fsx_xflags & FS_XFLAG_NODUMP);

	fa = orig;
	ASSERT_EQ(set_xattr(fd, &fa), 0);
	ASSERT_EQ(get_xattr(fd, &fa), 0);
	EXPECT_EQ(fa.fsx_xflags, orig.fsx_xflags);

	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(unlink(path), 0);
}

TEST_F(fileattr, set_noatime_roundtrip)
{
	char path[PATH_MAX];
	struct fsxattr fa, orig;
	int fd;

	ASSERT_EQ(build_path(path, sizeof(path), self->workdir, "noatime.XXXXXX"), 0);

	fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(close(fd), 0);

	fd = open_nofollow(path);
	ASSERT_GE(fd, 0);

	ASSERT_EQ(get_xattr(fd, &orig), 0);
	fa = orig;
	fa.fsx_xflags |= FS_XFLAG_NOATIME;
	ASSERT_EQ(set_xattr(fd, &fa), 0);
	ASSERT_EQ(get_xattr(fd, &fa), 0);
	EXPECT_TRUE(fa.fsx_xflags & FS_XFLAG_NOATIME);

	fa = orig;
	ASSERT_EQ(set_xattr(fd, &fa), 0);
	ASSERT_EQ(get_xattr(fd, &fa), 0);
	EXPECT_EQ(fa.fsx_xflags, orig.fsx_xflags);

	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(unlink(path), 0);
}

TEST_HARNESS_MAIN
