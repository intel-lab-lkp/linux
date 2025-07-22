// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Author: Aleksa Sarai <cyphar@cyphar.com>
 * Copyright (C) 2025 SUSE LLC.
 */

#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>

#include "../kselftest_harness.h"

#define bail(fmt, ...)							\
	do {								\
		fprintf(stderr, fmt ": %m", __VA_ARGS__);		\
		exit(1);						\
	} while (0)

#define ASSERT_SUCCESS	ASSERT_FALSE
#define ASSERT_FAIL	ASSERT_TRUE

int touch(char *path)
{
	int fd = open(path, O_WRONLY|O_CREAT|O_CLOEXEC, 0644);
	if (fd < 0 || close(fd) < 0)
		return -errno;
	return 0;
}

FIXTURE(ns)
{
	int host_mntns, host_pidns;
	int dummy_pidns;
};

FIXTURE_SETUP(ns)
{
	/* Stash the old mntns. */
	self->host_mntns = open("/proc/self/ns/mnt", O_RDONLY|O_CLOEXEC);
	ASSERT_GE(self->host_mntns, 0);

	/* Create a new mount namespace and make it private. */
	ASSERT_SUCCESS(unshare(CLONE_NEWNS));
	ASSERT_SUCCESS(mount(NULL, "/", NULL, MS_PRIVATE|MS_REC, NULL));

	/*
	 * Create a proper tmpfs that we can use and will disappear once we
	 * leave this mntns.
	 */
	ASSERT_SUCCESS(mount("tmpfs", "/tmp", "tmpfs", 0, NULL));

	/*
	 * Create a pidns we can use for later tests. We need to fork off a
	 * child so that we get a usable nsfd that we can bind-mount and open.
	 */
	ASSERT_SUCCESS(touch("/tmp/dummy-pidns"));

	self->host_pidns = open("/proc/self/ns/pid", O_RDONLY|O_CLOEXEC);
	ASSERT_GE(self->host_pidns, 0);
	ASSERT_SUCCESS(unshare(CLONE_NEWPID));

	pid_t pid = fork();
	ASSERT_GE(pid, 0);
	if (!pid) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		ASSERT_SUCCESS(mount("/proc/self/ns/pid", "/tmp/dummy-pidns", NULL, MS_BIND, 0));
		exit(0);
	}

	int wstatus;
	ASSERT_EQ(waitpid(pid, &wstatus, 0), pid);
	ASSERT_TRUE(WIFEXITED(wstatus));
	ASSERT_EQ(WEXITSTATUS(wstatus), 0);

	ASSERT_SUCCESS(setns(self->host_pidns, CLONE_NEWPID));

	self->dummy_pidns = open("/tmp/dummy-pidns", O_RDONLY|O_CLOEXEC);
	ASSERT_GE(self->dummy_pidns, 0);
}

FIXTURE_TEARDOWN(ns)
{
	ASSERT_SUCCESS(setns(self->host_mntns, CLONE_NEWNS));
	ASSERT_SUCCESS(close(self->host_mntns));

	ASSERT_SUCCESS(close(self->host_pidns));
	ASSERT_SUCCESS(close(self->dummy_pidns));
}

TEST_F(ns, pidns_mount_string_path)
{
	ASSERT_SUCCESS(mkdir("/tmp/proc-host", 0755));
	ASSERT_SUCCESS(mount("proc", "/tmp/proc-host", "proc", 0, "pidns=/proc/self/ns/pid"));
	ASSERT_SUCCESS(access("/tmp/proc-host/self/", X_OK));

	ASSERT_SUCCESS(mkdir("/tmp/proc-dummy", 0755));
	ASSERT_SUCCESS(mount("proc", "/tmp/proc-dummy", "proc", 0, "pidns=/tmp/dummy-pidns"));
	ASSERT_FAIL(access("/tmp/proc-dummy/1/", X_OK));
	ASSERT_FAIL(access("/tmp/proc-dummy/self/", X_OK));
}

TEST_F(ns, pidns_fsconfig_string_path)
{
	int fsfd = fsopen("proc", FSOPEN_CLOEXEC);
	ASSERT_GE(fsfd, 0);

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_SET_STRING, "pidns", "/tmp/dummy-pidns", 0));
	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0));

	int mountfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(mountfd, 0);

	ASSERT_FAIL(faccessat(mountfd, "1/", X_OK, 0));
	ASSERT_FAIL(faccessat(mountfd, "self/", X_OK, 0));

	ASSERT_SUCCESS(close(fsfd));
	ASSERT_SUCCESS(close(mountfd));
}

TEST_F(ns, pidns_fsconfig_fd)
{
	int fsfd = fsopen("proc", FSOPEN_CLOEXEC);
	ASSERT_GE(fsfd, 0);

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_SET_FD, "pidns", NULL, self->dummy_pidns));
	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0));

	int mountfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(mountfd, 0);

	ASSERT_FAIL(faccessat(mountfd, "1/", X_OK, 0));
	ASSERT_FAIL(faccessat(mountfd, "self/", X_OK, 0));

	ASSERT_SUCCESS(close(fsfd));
	ASSERT_SUCCESS(close(mountfd));
}

TEST_F(ns, pidns_reconfigure_remount)
{
	ASSERT_SUCCESS(mkdir("/tmp/proc", 0755));
	ASSERT_SUCCESS(mount("proc", "/tmp/proc", "proc", 0, ""));
	ASSERT_SUCCESS(access("/tmp/proc/self/", X_OK));

	ASSERT_SUCCESS(mount(NULL, "/tmp/proc", NULL, MS_REMOUNT, "pidns=/tmp/dummy-pidns"));
	ASSERT_FAIL(access("/tmp/proc/self/", X_OK));
}

TEST_F(ns, pidns_reconfigure_fsconfig_string_path)
{
	int fsfd = fsopen("proc", FSOPEN_CLOEXEC);
	ASSERT_GE(fsfd, 0);

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0));

	int mountfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(mountfd, 0);

	ASSERT_SUCCESS(faccessat(mountfd, "1/", X_OK, 0));
	ASSERT_SUCCESS(faccessat(mountfd, "self/", X_OK, 0));

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_SET_STRING, "pidns", "/tmp/dummy-pidns", 0));
	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0));

	ASSERT_FAIL(faccessat(mountfd, "1/", X_OK, 0));
	ASSERT_FAIL(faccessat(mountfd, "self/", X_OK, 0));

	ASSERT_SUCCESS(close(fsfd));
	ASSERT_SUCCESS(close(mountfd));
}

TEST_F(ns, pidns_reconfigure_fsconfig_fd)
{
	int fsfd = fsopen("proc", FSOPEN_CLOEXEC);
	ASSERT_GE(fsfd, 0);

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0));

	int mountfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(mountfd, 0);

	ASSERT_SUCCESS(faccessat(mountfd, "1/", X_OK, 0));
	ASSERT_SUCCESS(faccessat(mountfd, "self/", X_OK, 0));

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_SET_FD, "pidns", NULL, self->dummy_pidns));
	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0));

	ASSERT_FAIL(faccessat(mountfd, "1/", X_OK, 0));
	ASSERT_FAIL(faccessat(mountfd, "self/", X_OK, 0));

	ASSERT_SUCCESS(close(fsfd));
	ASSERT_SUCCESS(close(mountfd));
}

int is_same_inode(int fd1, int fd2)
{
	struct stat stat1, stat2;

	assert(fstat(fd1, &stat1) == 0);
	assert(fstat(fd2, &stat2) == 0);

	return stat1.st_ino == stat2.st_ino && stat1.st_dev == stat2.st_dev;
}

#define PROCFS_IOCTL_MAGIC 'f'
#define PROCFS_GET_PID_NAMESPACE	_IO(PROCFS_IOCTL_MAGIC, 1)

TEST_F(ns, get_pidns_ioctl)
{
	int fsfd = fsopen("proc", FSOPEN_CLOEXEC);
	ASSERT_GE(fsfd, 0);

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_SET_FD, "pidns", NULL, self->dummy_pidns));
	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0));

	int mountfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(mountfd, 0);

	/* fsmount returns an O_PATH, which ioctl(2) doesn't accept. */
	int new_mountfd = openat(mountfd, ".", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	ASSERT_GE(new_mountfd, 0);

	ASSERT_SUCCESS(close(mountfd));
	mountfd = -EBADF;

	int procfs_pidns = ioctl(new_mountfd, PROCFS_GET_PID_NAMESPACE);
	ASSERT_GE(procfs_pidns, 0);

	ASSERT_NE(self->dummy_pidns, procfs_pidns);
	ASSERT_FALSE(is_same_inode(self->host_pidns, procfs_pidns));
	ASSERT_TRUE(is_same_inode(self->dummy_pidns, procfs_pidns));

	ASSERT_SUCCESS(close(fsfd));
	ASSERT_SUCCESS(close(new_mountfd));
	ASSERT_SUCCESS(close(procfs_pidns));
}

TEST_F(ns, reconfigure_get_pidns_ioctl)
{
	int fsfd = fsopen("proc", FSOPEN_CLOEXEC);
	ASSERT_GE(fsfd, 0);

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_CREATE, NULL, NULL, 0));

	int mountfd = fsmount(fsfd, FSMOUNT_CLOEXEC, 0);
	ASSERT_GE(mountfd, 0);

	/* fsmount returns an O_PATH, which ioctl(2) doesn't accept. */
	int new_mountfd = openat(mountfd, ".", O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	ASSERT_GE(new_mountfd, 0);

	ASSERT_SUCCESS(close(mountfd));
	mountfd = -EBADF;

	int procfs_pidns1 = ioctl(new_mountfd, PROCFS_GET_PID_NAMESPACE);
	ASSERT_GE(procfs_pidns1, 0);

	ASSERT_NE(self->dummy_pidns, procfs_pidns1);
	ASSERT_TRUE(is_same_inode(self->host_pidns, procfs_pidns1));
	ASSERT_FALSE(is_same_inode(self->dummy_pidns, procfs_pidns1));

	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_SET_STRING, "pidns", "/tmp/dummy-pidns", 0));
	ASSERT_SUCCESS(fsconfig(fsfd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0));

	int procfs_pidns2 = ioctl(new_mountfd, PROCFS_GET_PID_NAMESPACE);
	ASSERT_GE(procfs_pidns2, 0);

	ASSERT_NE(self->dummy_pidns, procfs_pidns2);
	ASSERT_FALSE(is_same_inode(self->host_pidns, procfs_pidns2));
	ASSERT_TRUE(is_same_inode(self->dummy_pidns, procfs_pidns2));

	ASSERT_SUCCESS(close(fsfd));
	ASSERT_SUCCESS(close(new_mountfd));
	ASSERT_SUCCESS(close(procfs_pidns1));
	ASSERT_SUCCESS(close(procfs_pidns2));
}

TEST_HARNESS_MAIN
