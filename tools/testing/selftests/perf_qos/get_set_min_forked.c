// SPDX-License-Identifier: GPL-2.0-only
/*
 * Performance Quality of Service (Perf QoS) support base.
 *
 * Copyright (C) 2024 Linaro Ltd
 *
 * Author: Daniel Lezcano <daniel.lezcano@linaro.org>
 *
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/perf_qos_ioctl.h>

static int test_forked_get_set_min(int fd, const char *path)
{
	pid_t pid;
	int fds[2];
	int result;
	const int init_value = 256;
	struct perf_qos_ioctl_arg arg = { .value = init_value };

	if (ioctl(fd, PERF_QOS_IOC_SET_MIN, &arg)) {
		fprintf(stderr, "Failed to ioctl: %m\n");
		return 1;
	}

	if (pipe(fds)) {
		fprintf(stderr, "Failed to pipe: %m\n");
		return 1;
	}
		
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "Failed to fork: %m\n");
		return 1;
	}

	if (!pid) {
		close(fd);
		close(fds[0]);

		fd = open(path, 0, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "Failed to open '%s': %m\n", path);
			return 1;
		}

		arg.value = 0;

		/*
		 * At this point, we must have a 'init_value'
		 * constraint created by the parent process
		 */
		if (ioctl(fd, PERF_QOS_IOC_GET_MIN, &arg)) {
			fprintf(stderr, "Failed to ioctl: %m\n");
			return 1;
		}

		result = arg.value;

		if (write(fds[1], &result, sizeof(result)) < 0) {
			fprintf(stderr, "Failed to write result to pipe: %m\n");
			exit(1);
		}

		exit(0);
	}

	close(fds[1]);

	if (read(fds[0], &result, sizeof(result)) < 0) {
		fprintf(stderr, "Failed to read pipe: %m\n");
		return 1;
	}

	if (result != init_value) {
		fprintf(stderr, "Child test failed: %d\n", result);
		return 1;
	}

	if (waitpid(pid, NULL, 0) < 0) {
		fprintf(stderr, "Failed to wait child pid: %m\n");
		return 1;
	}

	arg.value = 0;

	if (ioctl(fd, PERF_QOS_IOC_GET_MIN, &arg)) {
		fprintf(stderr, "Failed to ioctl: %m\n");
		return 1;
	}

	if (arg.value != init_value) {
		fprintf(stderr, "Perf constraints differ %d <> %d\n",
			arg.value, init_value);
		return 1;
	}

	if (ioctl(fd, PERF_QOS_IOC_GET_LIMITS, &arg)) {
		fprintf(stderr, "Failed to ioctl: %m\n");
		return 1;
	}

	arg.value = arg.limit_min;

	if (ioctl(fd, PERF_QOS_IOC_SET_MIN, &arg)) {
		fprintf(stderr, "Failed to ioctl: %m\n");
		return 1;
	}

	if (!ioctl(fd, PERF_QOS_IOC_GET_MIN, &arg)) {
		fprintf(stderr, "ioctl should have failed\n");
		return 1;
	}

	if (errno != ENODATA) {
		fprintf(stderr, "errno should have been ENODATA\n");
		return 1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	const char *path = "/dev/perf_qos/dummy";
	int fd;
	
	if (argc == 2)
		path = argv[1];

	fd = open(path, 0, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open '%s': %m\n", path);
		return 1;
	}

	return test_forked_get_set_min(fd, path);
}
