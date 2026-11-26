// SPDX-License-Identifier: GPL-2.0
/*
 * The files in /proc/<pid>/fd, /proc/<pid>/fdinfo and below /proc/<pid>/net
 * can't be mounted on but they can be bind-mounted elsewhere.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "../wrappers.h"
#include "../../kselftest_harness.h"

#ifndef MOVE_MOUNT_BENEATH
#define MOVE_MOUNT_BENEATH 0x00000200
#endif

#define DIR_LEN 64
#define PATH_LEN 128

/* Open the procfs file itself, not the file it refers to. */
static int open_entry(const char *dir, int fd)
{
	char path[PATH_LEN];

	snprintf(path, sizeof(path), "/proc/self/%s/%d", dir, fd);
	return open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
}

static int bind_entry(int pfd, const char *target)
{
	char src[PATH_LEN];

	snprintf(src, sizeof(src), "/proc/self/fd/%d", pfd);
	return mount(src, target, NULL, MS_BIND, NULL);
}

/* An fdinfo file starts with the position of the file. */
static bool is_fdinfo(const char *path)
{
	char buf[5] = {};
	int fd;
	bool ret;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	ret = read(fd, buf, 4) == 4 && !strcmp(buf, "pos:");
	close(fd);
	return ret;
}

/* A shared tmpfs on @dir/a with a slave on @dir/b, both showing a file f. */
static int setup_slave(const char *dir, char *af, char *bf)
{
	char a[PATH_LEN], b[PATH_LEN];

	snprintf(a, sizeof(a), "%s/a", dir);
	snprintf(b, sizeof(b), "%s/b", dir);
	snprintf(af, PATH_LEN, "%s/a/f", dir);
	snprintf(bf, PATH_LEN, "%s/b/f", dir);
	if (mkdir(a, 0755) || mkdir(b, 0755) ||
	    mount("tmpfs", a, "tmpfs", 0, NULL) ||
	    mount(NULL, a, NULL, MS_SHARED, NULL) ||
	    mount(a, b, NULL, MS_BIND, NULL) ||
	    mount(NULL, b, NULL, MS_SLAVE, NULL))
		return -1;
	return mknod(af, S_IFREG | 0644, 0);
}

FIXTURE(procfs_source) {
	char dir[DIR_LEN];
	char file[PATH_LEN];
	int fd;
};

FIXTURE_SETUP(procfs_source)
{
	if (geteuid())
		SKIP(return, "test requires root");

	ASSERT_EQ(unshare(CLONE_NEWNS), 0);
	ASSERT_EQ(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL), 0);
	snprintf(self->dir, sizeof(self->dir), "/tmp/procfs_source.XXXXXX");
	ASSERT_NE(mkdtemp(self->dir), NULL);
	ASSERT_EQ(mount("tmpfs", self->dir, "tmpfs", 0, NULL), 0);
	snprintf(self->file, sizeof(self->file), "%s/file", self->dir);
	self->fd = open(self->file, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	ASSERT_GE(self->fd, 0);
}

FIXTURE_TEARDOWN(procfs_source)
{
	umount2(self->dir, MNT_DETACH);
	rmdir(self->dir);
}

TEST_F(procfs_source, bind_fd)
{
	struct stat st;
	int pfd;

	pfd = open_entry("fd", self->fd);
	ASSERT_GE(pfd, 0);
	ASSERT_EQ(bind_entry(pfd, self->file), 0);
	ASSERT_EQ(lstat(self->file, &st), 0);
	ASSERT_TRUE(S_ISLNK(st.st_mode));
	close(pfd);
}

TEST_F(procfs_source, bind_fdinfo)
{
	int pfd;

	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	ASSERT_EQ(bind_entry(pfd, self->file), 0);
	ASSERT_TRUE(is_fdinfo(self->file));
	close(pfd);
}

/* An entry below /proc/<pid>/net can't be mounted on either. */
TEST_F(procfs_source, bind_net)
{
	char buf[7] = {};
	int fd;

	if (access("/proc/self/net/dev", F_OK))
		SKIP(return, "no /proc/self/net/dev");
	ASSERT_EQ(mount("/proc/self/net/dev", self->file, NULL, MS_BIND, NULL), 0);
	fd = open(self->file, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(fd, 0);
	ASSERT_EQ(read(fd, buf, 6), 6);
	ASSERT_STREQ(buf, "Inter-");
	close(fd);
}

/* What libpathrs does to test that it notices overmounts in procfs. */
TEST_F(procfs_source, bind_fd_on_exe)
{
	char target[PATH_LEN];
	int pfd, exe;

	exe = open("/proc/self/exe", O_PATH | O_NOFOLLOW | O_CLOEXEC);
	ASSERT_GE(exe, 0);
	pfd = open_entry("fd", self->fd);
	ASSERT_GE(pfd, 0);
	snprintf(target, sizeof(target), "/proc/self/fd/%d", exe);
	ASSERT_EQ(bind_entry(pfd, target), 0);
	close(pfd);
	close(exe);
}

TEST_F(procfs_source, open_tree_fdinfo)
{
	int pfd, tfd;

	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	tfd = sys_open_tree(pfd, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	ASSERT_GE(tfd, 0);
	ASSERT_EQ(sys_move_mount(tfd, "", AT_FDCWD, self->file, MOVE_MOUNT_F_EMPTY_PATH), 0);
	ASSERT_TRUE(is_fdinfo(self->file));
	close(tfd);
	close(pfd);
}

TEST_F(procfs_source, no_mount_on_fdinfo)
{
	char target[PATH_LEN];
	int pfd;

	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	snprintf(target, sizeof(target), "/proc/self/fd/%d", pfd);
	ASSERT_NE(mount(self->file, target, NULL, MS_BIND, NULL), 0);
	ASSERT_EQ(errno, ENOENT);
	close(pfd);
}

/* Moving beneath would put the existing mount on top of the fdinfo file. */
TEST_F(procfs_source, no_tuck_beneath_fdinfo)
{
	char other[PATH_LEN];
	int pfd, tfd;

	snprintf(other, sizeof(other), "%s/other", self->dir);
	ASSERT_EQ(mknod(other, S_IFREG | 0644, 0), 0);
	ASSERT_EQ(mount(other, self->file, NULL, MS_BIND, NULL), 0);

	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	tfd = sys_open_tree(pfd, "", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_EMPTY_PATH);
	ASSERT_GE(tfd, 0);
	ASSERT_NE(sys_move_mount(tfd, "", AT_FDCWD, self->file,
				 MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_BENEATH), 0);
	ASSERT_EQ(errno, ENOENT);
	ASSERT_FALSE(is_fdinfo(self->file));
	ASSERT_EQ(umount2(self->file, 0), 0);
	ASSERT_NE(umount2(self->file, 0), 0);
	close(tfd);
	close(pfd);
}

/* A copy in a slave would be tucked beneath the slave's own mount. */
TEST_F(procfs_source, no_tuck_beneath_fdinfo_copy)
{
	char af[PATH_LEN], bf[PATH_LEN];
	int pfd;

	ASSERT_EQ(setup_slave(self->dir, af, bf), 0);
	ASSERT_EQ(mount(self->file, bf, NULL, MS_BIND, NULL), 0);

	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	ASSERT_NE(bind_entry(pfd, af), 0);
	ASSERT_EQ(errno, ENOENT);
	ASSERT_FALSE(is_fdinfo(af));

	/* Without the mount in the slave the copy goes there. */
	ASSERT_EQ(umount2(bf, 0), 0);
	ASSERT_EQ(bind_entry(pfd, af), 0);
	ASSERT_TRUE(is_fdinfo(af));
	ASSERT_TRUE(is_fdinfo(bf));
	close(pfd);
}

/* A peer gets its copy before the copy for the slave fails. */
TEST_F(procfs_source, no_tuck_beneath_fdinfo_after_peer)
{
	char af[PATH_LEN], bf[PATH_LEN], a[PATH_LEN], c[PATH_LEN], cf[PATH_LEN];
	int pfd;

	ASSERT_EQ(setup_slave(self->dir, af, bf), 0);
	snprintf(a, sizeof(a), "%s/a", self->dir);
	snprintf(c, sizeof(c), "%s/c", self->dir);
	snprintf(cf, sizeof(cf), "%s/c/f", self->dir);
	ASSERT_EQ(mkdir(c, 0755), 0);
	ASSERT_EQ(mount(a, c, NULL, MS_BIND, NULL), 0);
	ASSERT_EQ(mount(self->file, bf, NULL, MS_BIND, NULL), 0);

	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	ASSERT_NE(bind_entry(pfd, af), 0);
	ASSERT_EQ(errno, ENOENT);
	ASSERT_FALSE(is_fdinfo(af));
	ASSERT_FALSE(is_fdinfo(cf));

	ASSERT_EQ(umount2(bf, 0), 0);
	ASSERT_EQ(bind_entry(pfd, af), 0);
	ASSERT_TRUE(is_fdinfo(af));
	ASSERT_TRUE(is_fdinfo(bf));
	ASSERT_TRUE(is_fdinfo(cf));
	close(pfd);
}

/* Moved off a slave, its copy goes where the mount came from. */
TEST_F(procfs_source, move_fdinfo_off_slave)
{
	char af[PATH_LEN], bf[PATH_LEN];
	int pfd;

	ASSERT_EQ(setup_slave(self->dir, af, bf), 0);
	pfd = open_entry("fdinfo", self->fd);
	ASSERT_GE(pfd, 0);
	ASSERT_EQ(bind_entry(pfd, bf), 0);
	ASSERT_EQ(mount(bf, af, NULL, MS_MOVE, NULL), 0);
	ASSERT_TRUE(is_fdinfo(af));
	ASSERT_TRUE(is_fdinfo(bf));
	/* only the copy is left in the slave */
	ASSERT_EQ(umount2(bf, 0), 0);
	ASSERT_FALSE(is_fdinfo(bf));
	close(pfd);
}

TEST_HARNESS_MAIN
