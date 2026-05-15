// SPDX-License-Identifier: GPL-2.0
/*
 * Selftest for SECCOMP_IOCTL_NOTIF_INJECT.
 *
 * Exercises end-to-end syscall injection via the listener-fd
 * supervisor pattern: the child issues a filtered syscall, the
 * supervisor describes a substitute syscall in a kernel buffer, the
 * kernel runs the substitute on the child's behalf using kernel-mode
 * helpers (filp_open/kernel_bind/kernel_write), and the result lands
 * as the child's syscall return value. The trapped task's user mm is
 * never re-read for the substituted syscall.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../kselftest_harness.h"

#ifndef __NR_seccomp
#define __NR_seccomp 317
#endif

static int seccomp_install(int nr)
{
	struct sock_filter filter[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog prog = {
		.len = (unsigned short)ARRAY_SIZE(filter),
		.filter = filter,
	};

	return syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
}

/* ----------------------------------------------------------------
 * openat injection.
 * ----------------------------------------------------------------
 */
TEST(notif_inject_openat)
{
	char tmp_real[] = "/tmp/seccomp-inject-XXXXXX";
	int real_fd, listener, status;
	pid_t pid;

	real_fd = mkstemp(tmp_real);
	ASSERT_GE(real_fd, 0);
	ASSERT_EQ(write(real_fd, "real-data", 9), 9);
	ASSERT_EQ(close(real_fd), 0);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_openat);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		char readback[16] = {0};
		int fd;

		fd = openat(AT_FDCWD, "/this/path/does/not/exist", O_RDONLY);
		if (fd < 0)
			_exit(10);
		if (read(fd, readback, sizeof(readback) - 1) <= 0)
			_exit(11);
		_exit(memcmp(readback, "real-data", 9) == 0 ? 0 : 12);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};
	struct seccomp_notif_inject inj = {0};

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_openat);

	inj.id = req.id;
	inj.nr = __NR_openat;
	inj.args[0] = AT_FDCWD;
	inj.args[1] = 0;
	inj.args[2] = O_RDONLY;
	inj.args[3] = 0;
	inj.buf = (uintptr_t)tmp_real;
	inj.buf_size = strlen(tmp_real) + 1;
	inj.args_in_buf_mask = 1U << 1;
	ASSERT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), 0) {
		TH_LOG("INJECT failed: %s", strerror(errno));
	}

	resp.id = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_INJECTED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0) {
		TH_LOG("child exit status: %d", WEXITSTATUS(status));
	}

	unlink(tmp_real);
	close(listener);
}

/* ----------------------------------------------------------------
 * write injection.
 * Child issues write(fd, "agent-data", 10); supervisor injects
 * write(fd, "kernel-bytes", 12); verify file content matches the
 * kernel-injected bytes.
 * ----------------------------------------------------------------
 */
TEST(notif_inject_write)
{
	char path[] = "/tmp/seccomp-inject-write-XXXXXX";
	static const char inject_bytes[] = "kernel-bytes";
	int file_fd, listener, status;
	char file_content[32];
	pid_t pid;

	file_fd = mkstemp(path);
	ASSERT_GE(file_fd, 0);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_write);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		ssize_t n;

		n = write(file_fd, "agent-data", 10);
		_exit(n > 0 ? 0 : 10);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};
	struct seccomp_notif_inject inj = {0};

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_write);

	inj.id = req.id;
	inj.nr = __NR_write;
	inj.args[0] = req.data.args[0];   /* fd, pass-through */
	inj.args[1] = 0;
	inj.args[2] = strlen(inject_bytes);
	inj.buf = (uintptr_t)inject_bytes;
	inj.buf_size = strlen(inject_bytes);
	inj.args_in_buf_mask = 1U << 1;
	ASSERT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), 0);

	resp.id = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_INJECTED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);

	memset(file_content, 0, sizeof(file_content));
	ASSERT_EQ(lseek(file_fd, 0, SEEK_SET), 0);
	ASSERT_EQ(read(file_fd, file_content, sizeof(file_content) - 1),
		  (ssize_t)strlen(inject_bytes));
	EXPECT_EQ(memcmp(file_content, inject_bytes, strlen(inject_bytes)), 0) {
		TH_LOG("file content: '%s'", file_content);
	}

	close(file_fd);
	unlink(path);
	close(listener);
}

/* ----------------------------------------------------------------
 * bind injection.
 * ----------------------------------------------------------------
 */
TEST(notif_inject_bind)
{
	struct sockaddr_un real_addr = { .sun_family = AF_UNIX };
	char real_path[] = "/tmp/seccomp-inject-bind-XXXXXX";
	int listener, status;
	pid_t pid;

	mktemp(real_path);
	strcpy(real_addr.sun_path, real_path);
	unlink(real_addr.sun_path);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_bind);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		struct sockaddr_un fake = { .sun_family = AF_UNIX };
		int s;

		strcpy(fake.sun_path, "/tmp/seccomp-inject-bind-fake");
		s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s < 0)
			_exit(10);
		if (bind(s, (struct sockaddr *)&fake, sizeof(fake)) < 0)
			_exit(11);
		_exit(0);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};
	struct seccomp_notif_inject inj = {0};
	struct stat st;

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);
	EXPECT_EQ(req.data.nr, __NR_bind);

	inj.id = req.id;
	inj.nr = __NR_bind;
	inj.args[0] = req.data.args[0];   /* sockfd, pass-through */
	inj.args[1] = 0;
	inj.args[2] = sizeof(real_addr);
	inj.buf = (uintptr_t)&real_addr;
	inj.buf_size = sizeof(real_addr);
	inj.args_in_buf_mask = 1U << 1;
	ASSERT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), 0);

	resp.id = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_INJECTED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	EXPECT_TRUE(WIFEXITED(status));
	EXPECT_EQ(WEXITSTATUS(status), 0);

	/*
	 * The kernel-injected path should exist; the agent's intended
	 * path should not.
	 */
	EXPECT_EQ(stat(real_path, &st), 0);
	EXPECT_EQ(stat("/tmp/seccomp-inject-bind-fake", &st), -1);

	unlink(real_path);
	close(listener);
}

/* ----------------------------------------------------------------
 * Negative paths.
 * ----------------------------------------------------------------
 */
TEST(notif_inject_unsupported_syscall)
{
	int listener, status;
	pid_t pid;

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_close);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		close(99);
		_exit(0);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};
	struct seccomp_notif_inject inj = {0};

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	/* close() is not in the injectable whitelist. */
	inj.id = req.id;
	inj.nr = __NR_close;
	inj.args[0] = 99;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), -1);
	EXPECT_EQ(errno, EOPNOTSUPP);

	/* Cleanly deny so the child can exit. */
	resp.id = req.id;
	resp.error = -EBADF;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	close(listener);
}

TEST(notif_inject_syscall_mismatch)
{
	int listener, status;
	pid_t pid;

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_openat);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		openat(AT_FDCWD, "/nonexistent", O_RDONLY);
		_exit(0);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};
	struct seccomp_notif_inject inj = {0};

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	/* Trapped on openat, but supervisor tries to inject bind. */
	inj.id = req.id;
	inj.nr = __NR_bind;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), -1);
	EXPECT_EQ(errno, ESRCH);

	resp.id = req.id;
	resp.error = -EACCES;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);

	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	close(listener);
}

TEST(notif_inject_double)
{
	char tmp_real[] = "/tmp/seccomp-inject-d-XXXXXX";
	int real_fd, listener, status;
	pid_t pid;

	real_fd = mkstemp(tmp_real);
	ASSERT_GE(real_fd, 0);
	close(real_fd);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_openat);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		openat(AT_FDCWD, "/nonexistent", O_RDONLY);
		_exit(0);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};
	struct seccomp_notif_inject inj = {0};

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	inj.id = req.id;
	inj.nr = __NR_openat;
	inj.args[0] = AT_FDCWD;
	inj.args[1] = 0;
	inj.args[2] = O_RDONLY;
	inj.buf = (uintptr_t)tmp_real;
	inj.buf_size = strlen(tmp_real) + 1;
	inj.args_in_buf_mask = 1U << 1;
	ASSERT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), 0);

	/* Second INJECT for the same id is rejected. */
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_INJECT, &inj), -1);
	EXPECT_EQ(errno, EEXIST);

	resp.id = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_INJECTED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);
	EXPECT_EQ(waitpid(pid, &status, 0), pid);

	unlink(tmp_real);
	close(listener);
}

TEST(notif_inject_continue_pinned_conflict)
{
	int listener, status;
	pid_t pid;

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	listener = seccomp_install(__NR_openat);
	ASSERT_GE(listener, 0);

	pid = fork();
	ASSERT_GE(pid, 0);
	if (pid == 0) {
		openat(AT_FDCWD, "/nonexistent", O_RDONLY);
		_exit(0);
	}

	struct seccomp_notif req = {0};
	struct seccomp_notif_resp resp = {0};

	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req), 0);

	/* CONTINUE | INJECTED is rejected. */
	resp.id = req.id;
	resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE |
		     SECCOMP_USER_NOTIF_FLAG_INJECTED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), -1);
	EXPECT_EQ(errno, EINVAL);

	/* INJECTED without prior INJECT ioctl is rejected too. */
	resp.flags = SECCOMP_USER_NOTIF_FLAG_INJECTED;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), -1);
	EXPECT_EQ(errno, EINVAL);

	/* Cleanly deny so the child exits. */
	resp.flags = 0;
	resp.error = -ENOENT;
	EXPECT_EQ(ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp), 0);
	EXPECT_EQ(waitpid(pid, &status, 0), pid);
	close(listener);
}

TEST_HARNESS_MAIN
