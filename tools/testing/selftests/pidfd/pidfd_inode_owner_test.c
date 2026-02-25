// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/types.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "pidfd.h"
#include "kselftest_harness.h"

FIXTURE(pidfs_inode_owner)
{
	pid_t child_pid;
	int child_pidfd;
};

FIXTURE_SETUP(pidfs_inode_owner)
{
	int pipe_fds[2];
	char buf;

	self->child_pid = -1;
	self->child_pidfd = -1;

	ASSERT_EQ(pipe(pipe_fds), 0);

	self->child_pid = create_child(&self->child_pidfd, 0);
	ASSERT_GE(self->child_pid, 0);

	if (self->child_pid == 0) {
		close(pipe_fds[0]);
		write_nointr(pipe_fds[1], "c", 1);
		close(pipe_fds[1]);
		pause();
		_exit(EXIT_SUCCESS);
	}

	close(pipe_fds[1]);
	ASSERT_EQ(read_nointr(pipe_fds[0], &buf, 1), 1);
	close(pipe_fds[0]);
}

FIXTURE_TEARDOWN(pidfs_inode_owner)
{
	if (self->child_pid > 0) {
		kill(self->child_pid, SIGKILL);
		sys_waitid(P_PID, self->child_pid, NULL, WEXITED);
	}
	if (self->child_pidfd >= 0)
		close(self->child_pidfd);
}

/* Own pidfd reports correct ownership. */
TEST_F(pidfs_inode_owner, owner_self)
{
	int pidfd;
	struct stat st;

	pidfd = sys_pidfd_open(getpid(), 0);
	ASSERT_GE(pidfd, 0);

	ASSERT_EQ(fstat(pidfd, &st), 0);
	EXPECT_EQ(st.st_uid, getuid());
	EXPECT_EQ(st.st_gid, getgid());

	close(pidfd);
}

/* Child pidfd reports correct ownership. */
TEST_F(pidfs_inode_owner, owner_child)
{
	struct stat st;

	ASSERT_EQ(fstat(self->child_pidfd, &st), 0);
	EXPECT_EQ(st.st_uid, getuid());
	EXPECT_EQ(st.st_gid, getgid());
}

/* Ownership tracks credential changes in a live task. */
TEST_F(pidfs_inode_owner, owner_child_changed_uid)
{
	pid_t pid;
	int pidfd, pipe_fds[2];
	struct stat st;
	char buf;

	if (getuid() != 0)
		SKIP(return, "Test requires root");

	ASSERT_EQ(pipe(pipe_fds), 0);

	pid = create_child(&pidfd, 0);
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		close(pipe_fds[0]);
		if (setresgid(65534, 65534, 65534))
			_exit(PIDFD_ERROR);
		if (setresuid(65534, 65534, 65534))
			_exit(PIDFD_ERROR);
		write_nointr(pipe_fds[1], "c", 1);
		close(pipe_fds[1]);
		pause();
		_exit(EXIT_SUCCESS);
	}

	close(pipe_fds[1]);
	ASSERT_EQ(read_nointr(pipe_fds[0], &buf, 1), 1);
	close(pipe_fds[0]);

	ASSERT_EQ(fstat(pidfd, &st), 0);
	EXPECT_EQ(st.st_uid, (uid_t)65534);
	EXPECT_EQ(st.st_gid, (gid_t)65534);

	kill(pid, SIGKILL);
	sys_waitid(P_PID, pid, NULL, WEXITED);
	close(pidfd);
}

/* Ownership persists after the child exits and is reaped. */
TEST_F(pidfs_inode_owner, owner_exited_child)
{
	pid_t pid;
	int pidfd;
	struct stat st;

	pid = create_child(&pidfd, 0);
	ASSERT_GE(pid, 0);

	if (pid == 0)
		_exit(EXIT_SUCCESS);

	ASSERT_EQ(sys_waitid(P_PID, pid, NULL, WEXITED), 0);

	ASSERT_EQ(fstat(pidfd, &st), 0);
	EXPECT_EQ(st.st_uid, getuid());
	EXPECT_EQ(st.st_gid, getgid());

	close(pidfd);
}

/* Exit credentials preserve changed credentials. */
TEST_F(pidfs_inode_owner, owner_exited_child_changed_uid)
{
	pid_t pid;
	int pidfd;
	struct stat st;

	if (getuid() != 0)
		SKIP(return, "Test requires root");

	pid = create_child(&pidfd, 0);
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		if (setresgid(65534, 65534, 65534))
			_exit(PIDFD_ERROR);
		if (setresuid(65534, 65534, 65534))
			_exit(PIDFD_ERROR);
		_exit(EXIT_SUCCESS);
	}

	ASSERT_EQ(sys_waitid(P_PID, pid, NULL, WEXITED), 0);

	ASSERT_EQ(fstat(pidfd, &st), 0);
	EXPECT_EQ(st.st_uid, (uid_t)65534);
	EXPECT_EQ(st.st_gid, (gid_t)65534);

	close(pidfd);
}

/* Same-user cross-process permission check succeeds. */
TEST_F(pidfs_inode_owner, permission_same_user)
{
	pid_t pid;
	int pidfd;
	pid_t parent_pid = getpid();

	pid = create_child(&pidfd, 0);
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		int fd;
		char buf;

		fd = sys_pidfd_open(parent_pid, 0);
		if (fd < 0)
			_exit(PIDFD_ERROR);

		/*
		 * user.* xattr access triggers pidfs_permission().
		 * Same user's FSUID matches target's RUID, so
		 * generic_permission() passes and we get EOPNOTSUPP
		 * (no user.* xattr handler) instead of EACCES.
		 */
		if (fgetxattr(fd, "user.test", &buf, sizeof(buf)) < 0 &&
		    errno == EOPNOTSUPP) {
			close(fd);
			_exit(PIDFD_PASS);
		}

		close(fd);
		_exit(PIDFD_FAIL);
	}

	ASSERT_EQ(wait_for_pid(pid), PIDFD_PASS);
	close(pidfd);
}

/* Cross-user access is denied when FSUID doesn't match target's RUID. */
TEST_F(pidfs_inode_owner, permission_different_user_denied)
{
	pid_t pid;
	int pidfd;

	if (getuid() != 0)
		SKIP(return, "Test requires root");

	pid = create_child(&pidfd, 0);
	ASSERT_GE(pid, 0);

	if (pid == 0) {
		int fd;
		struct stat init_st;
		char buf;

		/* Open pidfd for init (uid 0). */
		fd = sys_pidfd_open(1, 0);
		if (fd < 0)
			_exit(PIDFD_ERROR);

		/* Verify init is actually uid 0 (may not be in all namespaces). */
		if (fstat(fd, &init_st) || init_st.st_uid != 0) {
			close(fd);
			_exit(PIDFD_SKIP);
		}

		/* Drop to uid/gid 65534 and lose all capabilities. */
		if (setresgid(65534, 65534, 65534))
			_exit(PIDFD_ERROR);
		if (setresuid(65534, 65534, 65534))
			_exit(PIDFD_ERROR);

		/*
		 * FSUID 65534 doesn't match target's RUID 0, and
		 * no CAP_DAC_OVERRIDE, so generic_permission()
		 * returns -EACCES.
		 */
		if (fgetxattr(fd, "user.test", &buf, sizeof(buf)) < 0 &&
		    errno == EACCES) {
			close(fd);
			_exit(PIDFD_PASS);
		}

		close(fd);
		_exit(PIDFD_FAIL);
	}

	{
		int ret = wait_for_pid(pid);
		if (ret == PIDFD_SKIP)
			SKIP(goto out, "pid 1 is not uid 0 (not in init PID namespace?)");
		ASSERT_EQ(ret, PIDFD_PASS);
	}
out:
	close(pidfd);
}

/* Kernel thread pidfd reports root ownership. */
TEST_F(pidfs_inode_owner, owner_kthread)
{
	int pidfd;
	struct stat st;
	char comm[16] = {};
	FILE *f;

	/*
	 * pid 2 is kthreadd only in the init PID namespace.
	 * Skip if we're in a different PID namespace.
	 */
	f = fopen("/proc/2/comm", "r");
	if (!f)
		SKIP(return, "Cannot read /proc/2/comm");
	if (!fgets(comm, sizeof(comm), f)) {
		fclose(f);
		SKIP(return, "Cannot read /proc/2/comm");
	}
	fclose(f);
	comm[strcspn(comm, "\n")] = '\0';
	if (strcmp(comm, "kthreadd") != 0)
		SKIP(return, "pid 2 is not kthreadd (not in init PID namespace?)");

	pidfd = sys_pidfd_open(2, 0);
	ASSERT_GE(pidfd, 0);

	ASSERT_EQ(fstat(pidfd, &st), 0);
	EXPECT_EQ(st.st_uid, (uid_t)0);
	EXPECT_EQ(st.st_gid, (gid_t)0);

	close(pidfd);
}

TEST_HARNESS_MAIN
