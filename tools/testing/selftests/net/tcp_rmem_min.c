// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "kselftest.h"

#define TCP_RMEM_PATH "/proc/sys/net/ipv4/tcp_rmem"

static int read_tcp_rmem(char *buf, size_t size)
{
	int fd = open(TCP_RMEM_PATH, O_RDONLY | O_CLOEXEC);
	ssize_t len;

	if (fd < 0)
		return -1;

	len = read(fd, buf, size - 1);
	close(fd);
	if (len < 0)
		return -1;

	buf[len] = '\0';
	return 0;
}

int main(void)
{
	/* tcp_rmem is min, default, max; keep min valid and lower the default. */
	static const char invalid_default[] = "4096 1 6291456";
	char before[128], after[128];
	ssize_t len;
	int fd, write_errno;

	ksft_print_header();
	ksft_set_plan(1);

	if (unshare(CLONE_NEWNET)) {
		if (errno == EPERM || errno == EACCES)
			ksft_exit_skip("network namespace unavailable\n");
		ksft_exit_fail_perror("unshare(CLONE_NEWNET)");
	}

	if (read_tcp_rmem(before, sizeof(before)))
		ksft_exit_fail_perror("read tcp_rmem before write");

	fd = open(TCP_RMEM_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		ksft_exit_fail_perror("open tcp_rmem");

	len = write(fd, invalid_default, sizeof(invalid_default) - 1);
	write_errno = errno;
	close(fd);

	if (len >= 0) {
		ksft_test_result_fail("tcp_rmem accepted a default below 4096\n");
		ksft_finished();
	}
	if (write_errno != EINVAL && write_errno != ERANGE) {
		errno = write_errno;
		ksft_exit_fail_perror("write tcp_rmem");
	}

	if (read_tcp_rmem(after, sizeof(after)))
		ksft_exit_fail_perror("read tcp_rmem after rejected write");
	if (strcmp(before, after)) {
		ksft_test_result_fail("tcp_rmem changed after rejecting invalid input\n");
		ksft_finished();
	}

	ksft_test_result_pass("tcp_rmem rejects a default below 4096\n");
	ksft_finished();
}
