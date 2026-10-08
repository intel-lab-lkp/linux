// SPDX-License-Identifier: GPL-2.0

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#define RECV_TIMEOUT_MS 150

struct recv_result {
	ssize_t result;
	int error;
};

int main(void)
{
	struct recv_result result;
	struct pollfd pfd;
	char byte = 'x';
	int sockets[2], pipefd[2], status;
	ssize_t n;
	pid_t child;

	ksft_print_header();
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
		ksft_exit_fail_perror("socketpair");
	if (send(sockets[0], &byte, sizeof(byte), MSG_OOB) != sizeof(byte)) {
		if (errno == EOPNOTSUPP || errno == EINVAL)
			ksft_exit_skip("AF_UNIX OOB support is unavailable\n");
		ksft_exit_fail_perror("send MSG_OOB");
	}
	if (pipe(pipefd))
		ksft_exit_fail_perror("pipe");

	ksft_set_plan(1);
	child = fork();
	if (child < 0)
		ksft_exit_fail_perror("fork");
	if (!child) {
		struct recv_result child_result;

		close(sockets[0]);
		close(pipefd[0]);
		errno = 0;
		child_result.result = recv(sockets[1], &byte, 0, MSG_PEEK);
		child_result.error = errno;
		if (write(pipefd[1], &child_result, sizeof(child_result)) !=
		    sizeof(child_result))
			_exit(1);
		_exit(0);
	}

	close(sockets[1]);
	close(pipefd[1]);
	pfd.fd = pipefd[0];
	pfd.events = POLLIN;
	n = poll(&pfd, 1, RECV_TIMEOUT_MS);
	if (n <= 0) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
		ksft_test_result_fail("zero-length MSG_PEEK did not return promptly\n");
		ksft_finished();
	}

	n = read(pipefd[0], &result, sizeof(result));
	if (n != sizeof(result)) {
		waitpid(child, &status, 0);
		ksft_test_result_fail("worker did not report the recv result\n");
		ksft_finished();
	}
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status)) {
		ksft_test_result_fail("worker did not exit successfully\n");
		ksft_finished();
	}
	if (result.result != 0) {
		errno = result.error;
		ksft_test_result_fail("zero-length MSG_PEEK returned an error\n");
		ksft_finished();
	}

	ksft_test_result_pass("zero-length MSG_PEEK returned immediately\n");
	ksft_finished();
}
