// SPDX-License-Identifier: GPL-2.0
/*
 * O_TMPFILE on vfat and msdos. run_fat_tests.sh mounts the image, passes the
 * mount point in FAT_MNT and sets FAT_DOTS=1 for msdos mounted with "dots".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <linux/msdos_fs.h>

#include "../../kselftest_harness.h"

/* Left behind for run_fat_tests.sh to check after a remount. */
#define TMPFILE_NAME	"tmpfile.txt"
#define REGULAR_NAME	"regular.txt"
#define HIDDEN_NAME	".hidden"

/* Clusters the space test writes. */
#define SPACE_CLUSTERS	8

/* Several clusters plus a partial one, a prime period so no two look alike. */
#define PERSIST_SIZE	(3 * 4096 + 123)
#define PERSIST_PERIOD	251

static const char data[] = "written through an O_TMPFILE\n";
static const char old_data[] = "old\n";

static int open_tmpfile(int dir_fd, int flags)
{
	return openat(dir_fd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC | flags, 0600);
}

static int link_tmpfile(int fd, int dir_fd, const char *name)
{
	return linkat(fd, "", dir_fd, name, AT_EMPTY_PATH);
}

static ssize_t read_file(int dir_fd, const char *name, char *buf, size_t size)
{
	ssize_t ret;
	int fd;

	fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = read(fd, buf, size);
	close(fd);
	return ret;
}

FIXTURE(tmpfile) {
	int mnt_fd;
	int dir_fd;
};

FIXTURE_SETUP(tmpfile)
{
	const char *mnt = getenv("FAT_MNT");
	char name[16];
	int fd;

	if (!mnt)
		SKIP(return, "FAT_MNT not set, run this from run_fat_tests.sh");

	self->mnt_fd = open(mnt, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	ASSERT_GE(self->mnt_fd, 0);

	/* A fresh directory per test, short enough for an 8.3 name. */
	snprintf(name, sizeof(name), "t%d", getpid());
	ASSERT_EQ(mkdirat(self->mnt_fd, name, 0755), 0);
	self->dir_fd = openat(self->mnt_fd, name,
			      O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	ASSERT_GE(self->dir_fd, 0);

	fd = open_tmpfile(self->dir_fd, 0);
	if (fd < 0 && errno == EOPNOTSUPP)
		SKIP(return, "O_TMPFILE not supported");
	ASSERT_GE(fd, 0);
	close(fd);
}

FIXTURE_TEARDOWN(tmpfile)
{
	close(self->dir_fd);
	close(self->mnt_fd);
}

/* An O_TMPFILE gets a name and keeps what was written to it. */
TEST_F(tmpfile, link)
{
	char buf[sizeof(data)];
	struct stat st;
	int fd;

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(fstat(fd, &st), 0);
	ASSERT_EQ(st.st_nlink, 0);
	ASSERT_EQ(write(fd, data, sizeof(data)), sizeof(data));

	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "file"), 0);
	ASSERT_EQ(fstat(fd, &st), 0);
	ASSERT_EQ(st.st_nlink, 1);
	ASSERT_EQ(st.st_size, sizeof(data));
	ASSERT_EQ(close(fd), 0);

	ASSERT_EQ(read_file(self->dir_fd, "file", buf, sizeof(buf)), sizeof(data));
	ASSERT_EQ(memcmp(buf, data, sizeof(data)), 0);
}

/* The name can be in another directory than the one O_TMPFILE was given. */
TEST_F(tmpfile, link_other_dir)
{
	char buf[sizeof(data)];
	int fd, sub_fd;

	ASSERT_EQ(mkdirat(self->dir_fd, "sub", 0755), 0);
	sub_fd = openat(self->dir_fd, "sub", O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	ASSERT_GE(sub_fd, 0);

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, data, sizeof(data)), sizeof(data));
	ASSERT_EQ(link_tmpfile(fd, sub_fd, "file"), 0);
	ASSERT_EQ(close(fd), 0);

	ASSERT_EQ(read_file(sub_fd, "file", buf, sizeof(buf)), sizeof(data));
	ASSERT_EQ(memcmp(buf, data, sizeof(data)), 0);
	close(sub_fd);
}

/* Data written after linkat() ends up in the named file too. */
TEST_F(tmpfile, write_after_link)
{
	char buf[2 * sizeof(data)];
	struct stat st;
	int fd;

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, data, sizeof(data)), sizeof(data));
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "file"), 0);
	ASSERT_EQ(write(fd, data, sizeof(data)), sizeof(data));
	ASSERT_EQ(close(fd), 0);

	ASSERT_EQ(fstatat(self->dir_fd, "file", &st, 0), 0);
	ASSERT_EQ(st.st_size, sizeof(buf));
	ASSERT_EQ(read_file(self->dir_fd, "file", buf, sizeof(buf)), sizeof(buf));
	ASSERT_EQ(memcmp(buf, data, sizeof(data)), 0);
	ASSERT_EQ(memcmp(buf + sizeof(data), data, sizeof(data)), 0);
}

/* O_EXCL keeps an O_TMPFILE from ever getting a name. */
TEST_F(tmpfile, excl)
{
	int fd;

	fd = open_tmpfile(self->dir_fd, O_EXCL);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "file"), -1);
	ASSERT_EQ(errno, ENOENT);
	close(fd);
}

/* Only the first name sticks. */
TEST_F(tmpfile, link_twice)
{
	int fd;

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "first"), 0);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "second"), -1);
	ASSERT_EQ(errno, EPERM);
	ASSERT_EQ(faccessat(self->dir_fd, "second", F_OK, 0), -1);
	ASSERT_EQ(errno, ENOENT);
	close(fd);
}

/* Once named and unlinked it is a plain unlinked file. */
TEST_F(tmpfile, relink_after_unlink)
{
	int fd;

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "file"), 0);
	ASSERT_EQ(unlinkat(self->dir_fd, "file", 0), 0);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "file"), -1);
	ASSERT_EQ(errno, ENOENT);
	close(fd);
}

/* A failed linkat() leaves the O_TMPFILE linkable. */
TEST_F(tmpfile, link_invalid_name)
{
	int fd;

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "bad*name"), -1);
	ASSERT_EQ(errno, EINVAL);
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "file"), 0);
	close(fd);
}

/* FAT still has no hard links. */
TEST_F(tmpfile, hardlink)
{
	int fd;

	fd = openat(self->dir_fd, "file", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
		    0600);
	ASSERT_GE(fd, 0);
	close(fd);
	ASSERT_EQ(linkat(self->dir_fd, "file", self->dir_fd, "link", 0), -1);
	ASSERT_EQ(errno, EPERM);
}

/* Link under a temporary name and rename over the target, as systemd does. */
TEST_F(tmpfile, replace)
{
	char buf[sizeof(data)];
	int fd;

	fd = openat(self->dir_fd, "target", O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
		    0600);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, old_data, sizeof(old_data)), sizeof(old_data));
	close(fd);

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, data, sizeof(data)), sizeof(data));
	ASSERT_EQ(link_tmpfile(fd, self->dir_fd, "target.tmp"), 0);
	ASSERT_EQ(renameat(self->dir_fd, "target.tmp", self->dir_fd, "target"), 0);
	ASSERT_EQ(close(fd), 0);

	ASSERT_EQ(read_file(self->dir_fd, "target", buf, sizeof(buf)), sizeof(data));
	ASSERT_EQ(memcmp(buf, data, sizeof(data)), 0);
}

/* Closing an unnamed O_TMPFILE gives its clusters back. */
TEST_F(tmpfile, space)
{
	struct statvfs before, during, after;
	size_t len;
	char *buf;
	int fd;

	ASSERT_EQ(fstatvfs(self->dir_fd, &before), 0);
	len = SPACE_CLUSTERS * before.f_bsize;
	buf = malloc(len);
	ASSERT_NE(buf, NULL);
	memset(buf, 'x', len);

	fd = open_tmpfile(self->dir_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, buf, len), len);
	ASSERT_EQ(fstatvfs(fd, &during), 0);
	ASSERT_LE(during.f_bfree, before.f_bfree - SPACE_CLUSTERS);
	ASSERT_EQ(close(fd), 0);

	ASSERT_EQ(fstatvfs(self->dir_fd, &after), 0);
	ASSERT_EQ(after.f_bfree, before.f_bfree);
	free(buf);
}

/* A named O_TMPFILE and a regular copy, compared after a remount. */
TEST_F(tmpfile, persist)
{
	char buf[PERSIST_SIZE];
	size_t half = sizeof(buf) / 2;
	size_t i;
	int fd;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = i % PERSIST_PERIOD;

	fd = openat(self->mnt_fd, REGULAR_NAME,
		    O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, buf, sizeof(buf)), sizeof(buf));
	ASSERT_EQ(fsync(fd), 0);
	ASSERT_EQ(close(fd), 0);

	fd = open_tmpfile(self->mnt_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(write(fd, buf, half), half);
	ASSERT_EQ(link_tmpfile(fd, self->mnt_fd, TMPFILE_NAME), 0);
	ASSERT_EQ(write(fd, buf + half, sizeof(buf) - half), sizeof(buf) - half);
	ASSERT_EQ(fsync(fd), 0);
	ASSERT_EQ(close(fd), 0);
	ASSERT_EQ(fsync(self->mnt_fd), 0);
}

/* On msdos with "dots" a leading dot makes the named O_TMPFILE hidden. */
TEST_F(tmpfile, hidden)
{
	const char *dots = getenv("FAT_DOTS");
	__u32 attrs;
	int fd;

	if (!dots || strcmp(dots, "1"))
		SKIP(return, "needs msdos mounted with the dots option");

	fd = open_tmpfile(self->mnt_fd, 0);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(link_tmpfile(fd, self->mnt_fd, HIDDEN_NAME), 0);
	ASSERT_EQ(ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &attrs), 0);
	ASSERT_TRUE(attrs & ATTR_HIDDEN);
	ASSERT_EQ(fsync(fd), 0);
	ASSERT_EQ(close(fd), 0);
	ASSERT_EQ(fsync(self->mnt_fd), 0);
}

TEST_HARNESS_MAIN
