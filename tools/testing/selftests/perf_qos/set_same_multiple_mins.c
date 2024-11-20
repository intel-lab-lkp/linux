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

int main(int argc, char *argv[])
{
	struct perf_qos_ioctl_arg arg = { .value = 512 };
	const char *path = "/dev/perf_qos/dummy";
	const int nr_fds = 256;
	int i, fd[nr_fds];

	if (argc == 2)
		path = argv[1];

	for (i = 0; i < nr_fds; i++) {
		fd[i] = open(path, 0, O_RDWR);
		if (fd[i] < 0) {
			fprintf(stderr, "Failed to open '%s': %m\n", path);
			return 1;
		}

		if (ioctl(fd[i], PERF_QOS_IOC_SET_MIN, &arg)) {
			fprintf(stderr, "Failed to ioctl: %m\n");
			return 1;
		}

		arg.value = 0;
		
		if (ioctl(fd[i], PERF_QOS_IOC_GET_MIN, &arg)) {
			fprintf(stderr, "Failed to ioctl: %m\n");
			return 1;
		}

		if (arg.value != 512) {
			fprintf(stderr, "min value differs with set/get (arg=%d)\n",
				arg.value);
			return 1;
		}
	}

	for (i = 0; i < nr_fds; i++)
		close(fd[i]);

	fd[0] = open(path, 0, O_RDWR);
	if (fd[0] < 0) {
		fprintf(stderr, "Failed to open '%s': %m\n", path);
		return 1;
	}

	/*
	 * Test: Check the constraint is removed
	 */
	if (!ioctl(fd[0], PERF_QOS_IOC_GET_MIN, &arg)) {
		fprintf(stderr, "ioctl should have failed\n");
		return 1;
	}

	if (errno != ENODATA) {
		fprintf(stderr, "errno should have been ENODATA\n");
		return 1;
	}

	return 0;
}
