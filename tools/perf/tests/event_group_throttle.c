// SPDX-License-Identifier: GPL-2.0
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <linux/perf_event.h>
#include "perf-sys.h"
#include "tests.h"
#include "debug.h"

static struct perf_event_attr attr_parent = {
	.type = PERF_TYPE_HARDWARE,
	.size = sizeof(attr_parent),
	.config = PERF_COUNT_HW_CPU_CYCLES,
	.sample_period = 1,
	.exclude_kernel = 1,
};

static struct perf_event_attr attr_child = {
	.type = PERF_TYPE_HARDWARE,
	.size = sizeof(attr_child),
	.config = PERF_COUNT_HW_CPU_CYCLES,
	.exclude_kernel = 1,
	.disabled = 1,
};

static pid_t run_event_group_throttle(void)
{
	pid_t pid = fork();

	if (pid == 0) {
		int parent, child;

		parent = sys_perf_event_open(&attr_parent, 0, -1, -1, 0);
		if (parent < 0) {
			pr_debug("Unable to create event: %d\n", parent);
			exit(-1);
		}

		child = sys_perf_event_open(&attr_child, 0, -1, parent, 0);
		if (child < 0) {
			pr_debug("Unable to create event: %d\n", child);
			exit(-1);
		}

		for (;;)
			asm("" ::: "memory");

		_exit(0);
	}
	return pid;
}

static bool is_kmsg_err(int fd)
{
	char buf[1024];
	ssize_t len;

	while ((len = read(fd, buf, sizeof(buf) - 1)) > 0) {
		buf[len] = '\0';

		if (strstr(buf, "UBSAN") || strstr(buf, "WARNING:") ||
		    strstr(buf, "BUG:") || strstr(buf, "Invalid PMEV")) {
			pr_debug("Kernel log error detected: %s", buf);
			return true;
		}
	}

	if (len < 0 && errno != EAGAIN) {
		pr_debug("Error reading /dev/kmsg: %s\n", strerror(errno));
		return true;
	}

	return false;
}

static int test__event_group_throttle(struct test_suite *test __maybe_unused,
				int subtest __maybe_unused)
{
	time_t start;
	pid_t pid;
	int fd;

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		/*
		 * If /dev/kmsg cannot be opened (e.g. permission denied), skip the test
		 * as we cannot verify the absence of kernel errors.
		 */
		pr_debug("Failed to open /dev/kmsg: %s. Skipping test.\n", strerror(errno));
		return TEST_SKIP;
	}

	/*
	 * Seek to the end to ignore past events (like EFI boot warnings).
	 * This typically requires CAP_SYSLOG.
	 */
	if (lseek(fd, 0, SEEK_END) < 0) {
		pr_debug("Failed to seek to end of /dev/kmsg: %s\n", strerror(errno));
		return TEST_FAIL;
	}

	start = time(NULL);
	do {
		pr_debug("Starting event group throttling...\n");
		pid = run_event_group_throttle();

		sleep(8);

		pr_debug("event group throttler(PID=%d)\n", pid);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);

		/* Check for errors during the run */
		if (is_kmsg_err(fd)) {
			close(fd);
			return TEST_FAIL;
		}
	} while (time(NULL) - start < 10);

	return TEST_OK;
}

DEFINE_SUITE("event group throttle", event_group_throttle);
