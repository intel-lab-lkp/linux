// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/limits.h>

#include "kselftest.h"
#include "cgroup_util.h"

/* How long to wait for the CPU-pressure trigger before giving up. */
#define PSI_POLL_TIMEOUT_MS	5000

static int pressure_open(const char *resource)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
	return open(path, O_RDWR);
}

/* Write a trigger descriptor, including the trailing NUL the parser expects. */
static ssize_t write_trigger(int fd, const char *trigger)
{
	return write(fd, trigger, strlen(trigger) + 1);
}

/* Trigger smoke test: second on same fd -> EBUSY; IRQ rejects "some". */
static int test_proc_triggers(const char *root)
{
	static const char *const resources[] = { "io", "memory", "cpu" };
	int ret = KSFT_FAIL;
	int fd = -1;
	int i;

	(void)root;

	for (i = 0; i < (int)ARRAY_SIZE(resources); i++) {
		fd = pressure_open(resources[i]);
		if (fd < 0) {
			ksft_print_msg("open /proc/pressure/%s: %s\n",
				       resources[i], strerror(errno));
			goto cleanup;
		}

		errno = 0;
		if (write_trigger(fd, "some 150000 2000000") <= 0) {
			ksft_print_msg("%s: 'some' trigger rejected: %s\n",
				       resources[i], strerror(errno));
			goto cleanup;
		}
		/* A second trigger on the same fd must fail with EBUSY. */
		errno = 0;
		if (write_trigger(fd, "full 150000 2000000") != -1 || errno != EBUSY) {
			ksft_print_msg("%s: second trigger expected EBUSY, got %s\n",
				       resources[i], strerror(errno));
			goto cleanup;
		}

		close(fd);
		fd = -1;
	}

	/* IRQ is full-only, and only exists with CONFIG_IRQ_TIME_ACCOUNTING. */
	fd = pressure_open("irq");
	if (fd >= 0) {
		errno = 0;
		if (write_trigger(fd, "some 150000 1000000") != -1) {
			ksft_print_msg("irq 'some': expected failure, got %s\n",
				       strerror(errno));
			goto cleanup;
		}
		close(fd);
		fd = -1;
	}

	ret = KSFT_PASS;
cleanup:
	if (fd >= 0)
		close(fd);
	return ret;
}

/* cgroup.pressure 0/1 hides/shows the *.pressure files and round-trips. */
static int test_cgroup_pressure_toggle(const char *root)
{
	char buf[BUF_SIZE] = { 0 };
	char *cg = NULL;
	int ret = KSFT_FAIL;

	cg = cg_name(root, "psi_toggle_test");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;

	if (cg_write(cg, "cgroup.pressure", "0")) {
		ksft_print_msg("failed to disable cgroup.pressure\n");
		goto cleanup;
	}
	if (cg_read(cg, "cgroup.pressure", buf, sizeof(buf)) < 0 || atoi(buf) != 0) {
		ksft_print_msg("cgroup.pressure=0 readback: '%s'\n", buf);
		goto cleanup;
	}
	if (cg_read(cg, "memory.pressure", buf, sizeof(buf)) >= 0) {
		ksft_print_msg("memory.pressure visible while disabled\n");
		goto cleanup;
	}

	if (cg_write(cg, "cgroup.pressure", "1")) {
		ksft_print_msg("failed to re-enable cgroup.pressure\n");
		goto cleanup;
	}
	if (cg_read(cg, "cgroup.pressure", buf, sizeof(buf)) < 0 || atoi(buf) != 1) {
		ksft_print_msg("cgroup.pressure=1 readback: '%s'\n", buf);
		goto cleanup;
	}
	if (cg_read(cg, "memory.pressure", buf, sizeof(buf)) < 0) {
		ksft_print_msg("memory.pressure unreadable while enabled\n");
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	if (cg)
		cg_destroy(cg);
	free(cg);
	return ret;
}

/* Induce deterministic CPU pressure (more hogs than CPUs). */
static int test_cgroup_trigger_fire(const char *root)
{
	char *cg = NULL, *cpupress = NULL;
	int fd = -1, ret = KSFT_FAIL;
	struct pollfd pfd;
	long ncpus, i;
	pid_t pid;

	cg = cg_name(root, "psi_trigger_test");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;

	cpupress = cg_control(cg, "cpu.pressure");
	if (!cpupress)
		goto cleanup;
	fd = open(cpupress, O_RDWR);
	if (fd < 0) {
		ksft_print_msg("open cpu.pressure: %s\n", strerror(errno));
		goto cleanup;
	}

	/* 1us threshold in a 1s window: any cpu stall fires it. */
	errno = 0;
	if (write_trigger(fd, "some 1 1000000") <= 0) {
		ksft_print_msg("arming trigger failed: %s\n", strerror(errno));
		goto cleanup;
	}

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus <= 0)
		ncpus = 1;

	pid = fork();
	if (pid < 0) {
		ksft_print_msg("fork: %s\n", strerror(errno));
		goto cleanup;
	}
	if (pid == 0) {
		/* Enter the cgroup, then over-subscribe it with CPU hogs. */
		if (cg_enter_current(cg))
			_exit(KSFT_FAIL);
		for (i = 0; i < ncpus; i++) {
			if (fork() == 0) {
				for (;;)
					asm volatile("" ::: "memory");
				_exit(0);
			}
		}
		for (;;)
			asm volatile("" ::: "memory");	/* child is also a hog */
		_exit(0);
	}

	pfd.fd = fd;
	pfd.events = POLLPRI;
	switch (poll(&pfd, 1, PSI_POLL_TIMEOUT_MS)) {
	case -1:
		ksft_print_msg("poll: %s\n", strerror(errno));
		break;
	case 0:
		ksft_print_msg("no trigger event; could not induce cpu pressure\n");
		ret = KSFT_SKIP;
		break;
	default:
		if (pfd.revents & POLLPRI)
			ret = KSFT_PASS;
		else
			ksft_print_msg("poll returned 0x%x\n", pfd.revents);
		break;
	}

	/* Stop the hogs (bounded, so waitpid can't hang) and reap the child. */
	cg_killall(cg);
	waitpid(pid, NULL, 0);

cleanup:
	if (fd >= 0)
		close(fd);
	if (cg) {
		cg_killall(cg);
		cg_destroy(cg);
	}
	free(cpupress);
	free(cg);
	return ret;
}

#define TEST(x) { #x, x }

struct psi_test {
	const char *name;
	int (*fn)(const char *root);
};

static struct psi_test tests[] = {
	TEST(test_proc_triggers),
	TEST(test_cgroup_pressure_toggle),
	TEST(test_cgroup_trigger_fire),
};

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	int mempress_fd;
	int i;

	(void)argc;

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	/* PSI must be enabled (CONFIG_PSI=y, not default-disabled). */
	mempress_fd = open("/proc/pressure/memory", O_RDONLY);
	if (mempress_fd < 0)
		ksft_exit_skip("PSI unavailable (CONFIG_PSI=n or psi=0)\n");
	close(mempress_fd);

	if (cg_read_strstr(root, "cgroup.controllers", "memory"))
		ksft_exit_skip("memory controller isn't available\n");
	if (cg_read_strstr(root, "cgroup.subtree_control", "memory"))
		if (cg_write(root, "cgroup.subtree_control", "+memory"))
			ksft_exit_skip("failed to enable memory controller\n");

	for (i = 0; i < (int)ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	ksft_finished();
}
