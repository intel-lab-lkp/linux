// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

#include "../kselftest.h"

#ifndef O_CRED_ALLOW
#define O_CRED_ALLOW 0x2000000
#endif

enum { FD_NORM, FD_CE, FD_CA, FD_MAX };

static int is_opened(int n)
{
	char buf[256];

	snprintf(buf, sizeof(buf), "/proc/self/fd/%d", n);
	return (access(buf, F_OK) == 0);
}

/* Sends an FD on a UNIX socket. Returns 0 on success or -errno. */
static int send_fd(int usock, int fd_tx)
{
	union {
		/* Aligned ancillary data buffer. */
		char buf[CMSG_SPACE(sizeof(fd_tx))];
		struct cmsghdr _align;
	} cmsg_tx = {};
	char data_tx = '.';
	struct iovec io = {
		.iov_base = &data_tx,
		.iov_len = sizeof(data_tx),
	};
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = &cmsg_tx.buf,
		.msg_controllen = sizeof(cmsg_tx.buf),
	};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	cmsg->cmsg_len = CMSG_LEN(sizeof(fd_tx));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(cmsg), &fd_tx, sizeof(fd_tx));

	if (sendmsg(usock, &msg, 0) < 0)
		return -errno;
	return 0;
}

int main(int argc, char *argv[], char *env[])
{
	int status;
	int err;
	pid_t pid;
	int socket_fds[2];
	int fds[FD_MAX];
#define NFD(n) ((n) + 3)
#define FD_OK(n) (NFD(n) == fds[n] && is_opened(fds[n]))

	if (argc > 1 && strcmp(argv[1], "--child") == 0) {
		int nfd = 0;
		ksft_print_msg("we are child\n");
		ksft_set_plan(3);
		nfd += is_opened(NFD(FD_NORM));
		ksft_test_result(nfd == 1, "normal fd opened\n");
		nfd += is_opened(NFD(FD_CE));
		ksft_test_result(nfd == 1, "O_CLOEXEC fd closed\n");
		nfd += is_opened(NFD(FD_CA));
		ksft_test_result(nfd == 1, "O_CRED_ALLOW fd closed\n");
		/* exit with non-zero status propagates to parent's failure */
		ksft_finished();
		return 0;
	}

	ksft_set_plan(7);

	fds[FD_NORM] = open("/proc/self/exe", O_RDONLY);
	fds[FD_CE] = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
	fds[FD_CA] = open("/proc/self/exe", O_RDONLY | O_CRED_ALLOW);
	ksft_test_result(FD_OK(FD_NORM), "regular open\n");
	ksft_test_result(FD_OK(FD_CE), "O_CLOEXEC open\n");
	ksft_test_result(FD_OK(FD_CA), "O_CRED_ALLOW open\n");

	err = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_fds);
	if (err) {
		ksft_perror("socketpair() failed");
		ksft_exit_fail_msg("socketpair\n");
		return 1;
	}
	err = send_fd(socket_fds[0], fds[FD_NORM]);
	ksft_test_result(err == 0, "normal fd sent\n");
	err = send_fd(socket_fds[0], fds[FD_CE]);
	ksft_test_result(err == 0, "O_CLOEXEC fd sent\n");
	err = send_fd(socket_fds[0], fds[FD_CA]);
	ksft_test_result(err == -EPERM, "O_CRED_ALLOW fd not sent, EPERM\n");
	close(socket_fds[0]);
	close(socket_fds[1]);

	pid = fork();
	if (pid < 0) {
		ksft_perror("fork() failed");
		ksft_exit_fail_msg("fork\n");
		return 1;
	}

	if (pid == 0) {
		char *cargv[] = {"cred_allow", "--child", NULL};

		execve("/proc/self/exe", cargv, env);
		ksft_perror("execve() failed");
		ksft_exit_fail_msg("execve\n");
		return 1;
	}

	if (waitpid(pid, &status, 0) != pid) {
		ksft_perror("waitpid() failed");
		ksft_exit_fail_msg("waitpid\n");
		return 1;
	}
	ksft_print_msg("back to parent\n");

	ksft_test_result(status == 0, "child success\n");

	ksft_finished();
	return 0;
}
