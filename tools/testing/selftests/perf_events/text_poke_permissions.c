// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

static int perf_event_open_text_poke(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_SOFTWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_SW_CPU_CLOCK,
		.disabled = 1,
		.exclude_kernel = 1,
		.text_poke = 1,
	};

	return syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
}

static int read_perf_paranoid(int *value)
{
	FILE *file;

	file = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
	if (!file)
		return -1;

	if (fscanf(file, "%d", value) != 1) {
		fclose(file);
		return -1;
	}
	fclose(file);
	return 0;
}

int main(void)
{
	pid_t child;
	int fd, status, paranoid;

	ksft_print_header();
	if (geteuid() != 0)
		ksft_exit_skip("run as root to verify the unprivileged permission check\n");

	if (read_perf_paranoid(&paranoid))
		ksft_exit_skip("cannot read perf_event_paranoid\n");
	if (paranoid <= 1)
		ksft_exit_skip("perf_event_paranoid must be greater than 1\n");

	fd = perf_event_open_text_poke();
	if (fd < 0) {
		if (errno == EINVAL || errno == E2BIG)
			ksft_exit_skip("kernel does not support perf text-poke events\n");
		if (errno == EACCES || errno == EPERM)
			ksft_exit_skip("root cannot open the probe perf event\n");
		ksft_exit_fail_perror("perf_event_open probe");
	}
	close(fd);

	ksft_set_plan(1);
	child = fork();
	if (child < 0)
		ksft_exit_fail_perror("fork");
	if (child == 0) {
		if (setgid(65534) || setuid(65534))
			_exit(2);
		fd = perf_event_open_text_poke();
		if (fd >= 0) {
			close(fd);
			_exit(0);
		}
		if (errno == EACCES || errno == EPERM)
			_exit(1);
		_exit(2);
	}

	if (waitpid(child, &status, 0) != child)
		ksft_exit_fail_perror("waitpid");
	if (!WIFEXITED(status))
		ksft_test_result_fail("permission-check child did not exit normally\n");
	else if (WEXITSTATUS(status) == 1)
		ksft_test_result_pass("unprivileged text-poke perf event is denied\n");
	else if (WEXITSTATUS(status) == 0)
		ksft_test_result_fail("unprivileged task opened a text-poke perf event\n");
	else
		ksft_test_result_fail("child failed to test text-poke permissions\n");

	ksft_finished();
}
