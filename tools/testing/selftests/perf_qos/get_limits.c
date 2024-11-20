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
	struct perf_qos_ioctl_arg arg = {};
	const char *path = "/dev/perf_qos/dummy";
	int fd;
	
	if (argc == 2)
		path = argv[1];

	fd = open(path, 0, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open '%s': %m\n", path);
		return 1;
	}

	if (ioctl(fd, PERF_QOS_IOC_GET_LIMITS, &arg)) {
		fprintf(stderr, "Failed to ioctl: %m\n");
		return 1;
	}

	if (arg.unit != PERF_QOS_UNIT_NORMAL) {
		fprintf(stderr, "Invalid unit, expected 'PERF_QOS_UNIT_NORMAL'\n");
		return 1;
	}

	if (arg.limit_min != 0) {
		fprintf(stderr, "Invalid minimum unit %d != 0\n", arg.limit_min);
		return 1;
	}

	if (arg.limit_max != 1024) {
		fprintf(stderr, "Invalid maximum unit %d != 1024\n", arg.limit_max);
		return 1;
	}

	return 0;
}
