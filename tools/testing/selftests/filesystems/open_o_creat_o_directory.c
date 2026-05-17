// SPDX-License-Identifier: GPL-2.0
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>

#include "kselftest_harness.h"

#define mkdirat_fd_checked_flags(dfd, pathname, flags) ({	\
	struct stat __st;					\
	int __fd = mkdirat_fd(dfd, pathname, S_IRWXU, flags);	\
	ASSERT_GE(__fd, 0);					\
	EXPECT_EQ(fstat(__fd, &__st), 0);			\
	EXPECT_TRUE(S_ISDIR(__st.st_mode));			\
	__fd;							\
})

#define mkdirat_fd_checked(dfd, pathname) \
	mkdirat_fd_checked_flags(dfd, pathname, 0)

static inline int mkdirat_fd(int dfd, const char *pathname, mode_t mode,
                             unsigned int flags)
{
    return syscall(__NR_openat, dfd, pathname,
                   flags | O_DIRECTORY | O_CREAT, mode);
}

FIXTURE(mkdirat_fd) {
	char dirpath[PATH_MAX];
	int dfd;
};

FIXTURE_SETUP(mkdirat_fd)
{
	snprintf(self->dirpath, sizeof(self->dirpath),
		 "/tmp/mkdirat_fd_test.%d", getpid());
	ASSERT_EQ(mkdir(self->dirpath, S_IRWXU), 0);

	self->dfd = open(self->dirpath, O_DIRECTORY);
	ASSERT_GE(self->dfd, 0);
}

FIXTURE_TEARDOWN(mkdirat_fd)
{
	close(self->dfd);
	rmdir(self->dirpath);
}

/* Does mkdirat_fd return a fd at all */
TEST_F(mkdirat_fd, returns_fd)
{
	int fd = mkdirat_fd_checked(self->dfd, "newdir");
	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(unlinkat(self->dfd, "newdir", AT_REMOVEDIR), 0);
}

/* The fd must refer to the directory that was just created. */
TEST_F(mkdirat_fd, fd_is_created_dir)
{
	int fd;
	struct stat st_via_fd, st_via_path;
	char path[PATH_MAX];

	fd = mkdirat_fd_checked(self->dfd, "checkdir");

	ASSERT_EQ(fstat(fd, &st_via_fd), 0);

	snprintf(path, sizeof(path), "%s/checkdir", self->dirpath);
	ASSERT_EQ(stat(path, &st_via_path), 0);

	EXPECT_EQ(st_via_fd.st_ino, st_via_path.st_ino);
	EXPECT_EQ(st_via_fd.st_dev, st_via_path.st_dev);

	EXPECT_EQ(close(fd), 0);
	EXPECT_EQ(rmdir(path), 0);
}


/* Missing parent component must fail with ENOENT. */
TEST_F(mkdirat_fd, enoent_missing_parent)
{
	EXPECT_EQ(mkdirat_fd(self->dfd, "nonexistent/child", S_IRWXU, 0), -1);
	EXPECT_EQ(errno, ENOENT);
}

/* An invalid dfd must fail with EBADF. */
TEST_F(mkdirat_fd, ebadf)
{
	EXPECT_EQ(mkdirat_fd(-42, "badfdir", S_IRWXU, 0), -1);
	EXPECT_EQ(errno, EBADF);
}

/* A dfd that points to a file (not a directory) must fail with ENOTDIR. */
TEST_F(mkdirat_fd, enotdir_dfd)
{
	int file_fd;

	file_fd = openat(self->dfd, "file",
			 O_CREAT | O_WRONLY, S_IRWXU);
	ASSERT_GE(file_fd, 0);

	EXPECT_EQ(mkdirat_fd(file_fd, "subdir", S_IRWXU, 0), -1);
	EXPECT_EQ(errno, ENOTDIR);

	EXPECT_EQ(close(file_fd), 0);
	EXPECT_EQ(unlinkat(self->dfd, "file", 0), 0);
}

/*
 * O_EXCL together with O_CREAT|O_DIRECTORY must fail with EEXIST when
 * the target directory already exists.
 */
TEST_F(mkdirat_fd, o_excl_eexist)
{
	int fd;

	fd = mkdirat_fd_checked_flags(self->dfd, "excldir", O_EXCL);
	EXPECT_EQ(close(fd), 0);

	EXPECT_EQ(mkdirat_fd(self->dfd, "excldir", S_IRWXU, O_EXCL), -1);
	EXPECT_EQ(errno, EEXIST);

	EXPECT_EQ(unlinkat(self->dfd, "excldir", AT_REMOVEDIR), 0);
}

/*
 * The returned fd must be usable as a dfd for further *at() calls.
 */
TEST_F(mkdirat_fd, fd_usable_as_dfd)
{
	int parent_fd, child_fd;

	parent_fd = mkdirat_fd_checked(self->dfd, "parent");
	child_fd = mkdirat_fd_checked(parent_fd, "child");

	EXPECT_EQ(close(child_fd), 0);
	EXPECT_EQ(close(parent_fd), 0);

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/parent/child", self->dirpath);
	EXPECT_EQ(rmdir(path), 0);
	snprintf(path, sizeof(path), "%s/parent", self->dirpath);
	EXPECT_EQ(rmdir(path), 0);
}

TEST_HARNESS_MAIN
