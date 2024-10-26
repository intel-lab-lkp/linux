// SPDX-License-Identifier: GPL-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/types.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <linux/kcmp.h>

#include "pidfd.h"
#include "../kselftest_harness.h"

/*
 * UNKNOWN_FD is an fd number that should never exist in the child, as it is
 * used to check the negative case.
 */
#define UNKNOWN_FD 111
#define UID_NOBODY 65535

static int sys_kcmp(pid_t pid1, pid_t pid2, int type, unsigned long idx1,
		    unsigned long idx2)
{
	return syscall(__NR_kcmp, pid1, pid2, type, idx1, idx2);
}

static int __child(int sk, int memfd)
{
	int ret;
	char buf;

	/*
	 * Ensure we don't leave around a bunch of orphaned children if our
	 * tests fail.
	 */
	ret = prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (ret) {
		fprintf(stderr, "%s: Child could not set DEATHSIG\n",
			strerror(errno));
		return -1;
	}

	ret = send(sk, &memfd, sizeof(memfd), 0);
	if (ret != sizeof(memfd)) {
		fprintf(stderr, "%s: Child failed to send fd number\n",
			strerror(errno));
		return -1;
	}

	/*
	 * The fixture setup is completed at this point. The tests will run.
	 *
	 * This blocking recv enables the parent to message the child.
	 * Either we will read 'P' off of the sk, indicating that we need
	 * to disable ptrace, or we will read a 0, indicating that the other
	 * side has closed the sk. This occurs during fixture teardown time,
	 * indicating that the child should exit.
	 */
	while ((ret = recv(sk, &buf, sizeof(buf), 0)) > 0) {
		if (buf == 'P') {
			ret = prctl(PR_SET_DUMPABLE, 0);
			if (ret < 0) {
				fprintf(stderr,
					"%s: Child failed to disable ptrace\n",
					strerror(errno));
				return -1;
			}
		} else {
			fprintf(stderr, "Child received unknown command %c\n",
				buf);
			return -1;
		}
		ret = send(sk, &buf, sizeof(buf), 0);
		if (ret != 1) {
			fprintf(stderr, "%s: Child failed to ack\n",
				strerror(errno));
			return -1;
		}
	}
	if (ret < 0) {
		fprintf(stderr, "%s: Child failed to read from socket\n",
			strerror(errno));
		return -1;
	}

	return 0;
}

static int child(int sk)
{
	int memfd, ret;

	memfd = sys_memfd_create("test", 0);
	if (memfd < 0) {
		fprintf(stderr, "%s: Child could not create memfd\n",
			strerror(errno));
		ret = -1;
	} else {
		ret = __child(sk, memfd);
		close(memfd);
	}

	close(sk);
	return ret;
}

static int __pidfd_self_thread_worker(unsigned long page_size)
{
	int memfd;
	int newfd;
	char *ptr;
	int err = 0;

	/*
	 * Unshare our FDs so we have our own set. This means
	 * PIDFD_SELF_THREAD_GROUP will fal.
	 */
	if (unshare(CLONE_FILES) < 0) {
		err = -errno;
		goto exit;
	}

	/* Truncate, map in and write to our memfd. */
	memfd = sys_memfd_create("test_self_child", 0);
	if (memfd < 0) {
		err = -errno;
		goto exit;
	}

	if (ftruncate(memfd, page_size)) {
		err = -errno;
		goto exit_close_memfd;
	}

	ptr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, memfd, 0);
	if (ptr == MAP_FAILED) {
		err = -errno;
		goto exit_close_memfd;
	}
	ptr[0] = 'y';
	if (munmap(ptr, page_size)) {
		err = -errno;
		goto exit_close_memfd;
	}

	/* Get a thread-local duplicate of our memfd. */
	newfd = sys_pidfd_getfd(PIDFD_SELF_THREAD, memfd, 0);
	if (newfd < 0) {
		err = -errno;
		goto exit_close_memfd;
	}

	if (memfd == newfd) {
		err = -EINVAL;
		goto exit_close_fds;
	}

	/* Map in new fd and make sure that the data is as expected. */
	ptr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, newfd, 0);
	if (ptr == MAP_FAILED) {
		err = -errno;
		goto exit_close_fds;
	}

	if (ptr[0] != 'y') {
		err = -EINVAL;
		goto exit_close_fds;
	}

	if (munmap(ptr, page_size)) {
		err = -errno;
		goto exit_close_fds;
	}

exit_close_fds:
	close(newfd);
exit_close_memfd:
	close(memfd);
exit:
	return err;
}

static void *pidfd_self_thread_worker(void *arg)
{
	unsigned long page_size = (unsigned long)arg;
	int ret;

	/* We forward any errors for the caller to handle. */
	ret = __pidfd_self_thread_worker(page_size);
	return (void *)(intptr_t)ret;
}

FIXTURE(child)
{
	/*
	 * remote_fd is the number of the FD which we are trying to retrieve
	 * from the child.
	 */
	int remote_fd;
	/* pid points to the child which we are fetching FDs from */
	pid_t pid;
	/* pidfd is the pidfd of the child */
	int pidfd;
	/*
	 * sk is our side of the socketpair used to communicate with the child.
	 * When it is closed, the child will exit.
	 */
	int sk;
	bool ignore_child_result;
};

FIXTURE_SETUP(child)
{
	int ret, sk_pair[2];

	ASSERT_EQ(0, socketpair(PF_LOCAL, SOCK_SEQPACKET, 0, sk_pair)) {
		TH_LOG("%s: failed to create socketpair", strerror(errno));
	}
	self->sk = sk_pair[0];

	self->pid = fork();
	ASSERT_GE(self->pid, 0);

	if (self->pid == 0) {
		close(sk_pair[0]);
		if (child(sk_pair[1]))
			_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}

	close(sk_pair[1]);

	self->pidfd = sys_pidfd_open(self->pid, 0);
	ASSERT_GE(self->pidfd, 0);

	/*
	 * Wait for the child to complete setup. It'll send the remote memfd's
	 * number when ready.
	 */
	ret = recv(sk_pair[0], &self->remote_fd, sizeof(self->remote_fd), 0);
	ASSERT_EQ(sizeof(self->remote_fd), ret);
}

FIXTURE_TEARDOWN(child)
{
	int ret;

	EXPECT_EQ(0, close(self->pidfd));
	EXPECT_EQ(0, close(self->sk));

	ret = wait_for_pid(self->pid);
	if (!self->ignore_child_result)
		EXPECT_EQ(0, ret);
}

TEST_F(child, disable_ptrace)
{
	int uid, fd;
	char c;

	/*
	 * Turn into nobody if we're root, to avoid CAP_SYS_PTRACE
	 *
	 * The tests should run in their own process, so even this test fails,
	 * it shouldn't result in subsequent tests failing.
	 */
	uid = getuid();
	if (uid == 0)
		ASSERT_EQ(0, seteuid(UID_NOBODY));

	ASSERT_EQ(1, send(self->sk, "P", 1, 0));
	ASSERT_EQ(1, recv(self->sk, &c, 1, 0));

	fd = sys_pidfd_getfd(self->pidfd, self->remote_fd, 0);
	EXPECT_EQ(-1, fd);
	EXPECT_EQ(EPERM, errno);

	if (uid == 0)
		ASSERT_EQ(0, seteuid(0));
}

TEST_F(child, fetch_fd)
{
	int fd, ret;

	fd = sys_pidfd_getfd(self->pidfd, self->remote_fd, 0);
	ASSERT_GE(fd, 0);

	ret = sys_kcmp(getpid(), self->pid, KCMP_FILE, fd, self->remote_fd);
	if (ret < 0 && errno == ENOSYS)
		SKIP(return, "kcmp() syscall not supported");
	EXPECT_EQ(ret, 0);

	ret = fcntl(fd, F_GETFD);
	ASSERT_GE(ret, 0);
	EXPECT_GE(ret & FD_CLOEXEC, 0);

	close(fd);
}

TEST_F(child, test_unknown_fd)
{
	int fd;

	fd = sys_pidfd_getfd(self->pidfd, UNKNOWN_FD, 0);
	EXPECT_EQ(-1, fd) {
		TH_LOG("getfd succeeded while fetching unknown fd");
	};
	EXPECT_EQ(EBADF, errno) {
		TH_LOG("%s: getfd did not get EBADF", strerror(errno));
	}
}

TEST(flags_set)
{
	ASSERT_EQ(-1, sys_pidfd_getfd(0, 0, 1));
	EXPECT_EQ(errno, EINVAL);
}

TEST_F(child, no_strange_EBADF)
{
	struct pollfd fds;

	self->ignore_child_result = true;

	fds.fd = self->pidfd;
	fds.events = POLLIN;

	ASSERT_EQ(kill(self->pid, SIGKILL), 0);
	ASSERT_EQ(poll(&fds, 1, 5000), 1);

	/*
	 * It used to be that pidfd_getfd() could race with the exiting thread
	 * between exit_files() and release_task(), and get a non-null task
	 * with a NULL files struct, and you'd get EBADF, which was slightly
	 * confusing.
	 */
	errno = 0;
	EXPECT_EQ(sys_pidfd_getfd(self->pidfd, self->remote_fd, 0), -1);
	EXPECT_EQ(errno, ESRCH);
}

TEST(pidfd_self)
{
	int memfd = sys_memfd_create("test_self", 0);
	unsigned long page_size = sysconf(_SC_PAGESIZE);
	int newfd;
	char *ptr;
	pthread_t thread;
	void *res;
	int err;

	ASSERT_GE(memfd, 0);
	ASSERT_EQ(ftruncate(memfd, page_size), 0);

	/*
	 * Map so we can assert that the duplicated fd references the same
	 * memory.
	 */
	ptr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, memfd, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	ptr[0] = 'x';
	ASSERT_EQ(munmap(ptr, page_size), 0);

	/* Now get a duplicate of our memfd. */
	newfd = sys_pidfd_getfd(PIDFD_SELF_THREAD_GROUP, memfd, 0);
	ASSERT_GE(newfd, 0);
	ASSERT_NE(memfd, newfd);

	/* Now map duplicate fd and make sure it references the same memory. */
	ptr = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, newfd, 0);
	ASSERT_NE(ptr, MAP_FAILED);
	ASSERT_EQ(ptr[0], 'x');
	ASSERT_EQ(munmap(ptr, page_size), 0);

	/* Cleanup. */
	close(memfd);
	close(newfd);

	/*
	 * Fire up the thread and assert that we can lookup the thread-specific
	 * PIDFD_SELF_THREAD (also aliased by PIDFD_SELF).
	 */
	ASSERT_EQ(pthread_create(&thread, NULL, pidfd_self_thread_worker,
				 (void *)page_size), 0);
	ASSERT_EQ(pthread_join(thread, &res), 0);
	err = (int)(intptr_t)res;

	ASSERT_EQ(err, 0);
}

#if __NR_pidfd_getfd == -1
int main(void)
{
	fprintf(stderr, "__NR_pidfd_getfd undefined. The pidfd_getfd syscall is unavailable. Test aborting\n");
	return KSFT_SKIP;
}
#else
TEST_HARNESS_MAIN
#endif
