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

static int integer_cmp(const void *a, const void *b)
{
	int *ia = (typeof(ia))(a);
	int *ib = (typeof(ib))(b);

	return (*ia) - (*ib);
}

static int test_forked_set_max(int fd, const char *path)
{
	struct perf_qos_ioctl_arg arg;

	int i;
	int nr_pids = 100;
	pid_t pids[nr_pids];
	int value, values[nr_pids];
	int fds[nr_pids][2];
	int ret = 1;
	
	memset(pids, 0, sizeof(pid_t) * nr_pids);

	/*
	 * Random values in the interval 0-1024, to be set by each
	 * child process. The underlying framework will sort them out
	 * so when reading them, they should be ordered and while the
	 * child process exits, the new maximal will be set each time.
	 */
	for (i = 0; i < nr_pids; i++) {
		value = rand() % 1023;

		if (pipe(fds[i])) {
			fprintf(stderr, "Failed to pipe: %m\n");
			goto out;
		}
		
		pids[i] = fork();
		if (pids[i] < 0) {
			fprintf(stderr, "Failed to fork: %m\n");
			goto out;
		}

		if (!pids[i]) {

			arg.value = value;

			close(fd);
			close(fds[i][0]);

			fd = open(path, 0, O_RDWR);
			if (fd < 0) {
				fprintf(stderr, "Failed to open '%s': %m\n", path);
				goto out;
			}

			if (ioctl(fd, PERF_QOS_IOC_SET_MAX, &arg)) {
				fprintf(stderr, "Failed to ioctl: %m\n");
				goto out;
			}

			if (write(fds[i][1], &value, sizeof(value)) < 0) {
				fprintf(stderr, "Failed to write in the pipe: %m\n");
				goto out;
			}

			poll(0, 0, -1);
			
			exit(0);
		}

		close(fds[i][1]);
		values[i] = value;
	}

	/*
	 * Wait for all the children to set the constraint and write
	 * to the pipe
	 */
	for (i = 0; i < nr_pids; i++) {
		if (read(fds[i][0], &value, sizeof(value)) < 0) {
			fprintf(stderr, "Failed to read pipe: %m\n");
			goto out;
		}
	}

	qsort(values, nr_pids, sizeof(values[0]), integer_cmp);

	if (ioctl(fd, PERF_QOS_IOC_GET_MAX, &arg)) {
		fprintf(stderr, "Failed to ioctl: %m\n");
		goto out;
	}

	if (arg.value != values[0]) {
		fprintf(stderr, "Unexcepted value order %d <> %d\n",
			arg.value, values[0]);
		goto out;
	}

	ret = 0;
out:
	for (i = 0; i < nr_pids; i++) {
		kill(pids[i], SIGTERM);
		waitpid(pids[i], NULL, 0);
	}

	return ret;
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

	return test_forked_set_max(fd, path);
}
