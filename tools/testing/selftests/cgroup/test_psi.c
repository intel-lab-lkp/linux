// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/limits.h>

#include "kselftest.h"
#include "cgroup_util.h"

/* How long to wait for a best-effort trigger event before giving up. */
#define PSI_POLL_TIMEOUT_MS	5000
/* Memory footprint of the churner; much larger than the cgroup's memory.max. */
#define PSI_CHURN_SIZE		MB(128)

/* Open /proc/pressure/<resource> O_RDWR (write registers triggers). */
static int pressure_open(const char *resource)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
	return open(path, O_RDWR);
}

/* Write trigger descriptor with the trailing NUL the parser expects. */
static ssize_t write_trigger(int fd, const char *trigger)
{
	return write(fd, trigger, strlen(trigger) + 1);
}

/* 2nd trigger on same fd -> EBUSY; bad window/threshold/garbage -> EINVAL. */
static int test_proc_pressure_validation(const char *root)
{
	static const char *const resources[] = { "cpu", "memory", "io" };
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

		/* 2s window works with or without CAP_SYS_RESOURCE. */
		errno = 0;
		if (write_trigger(fd, "some 150000 2000000") <= 0) {
			ksft_print_msg("valid 'some' trigger rejected: %s\n",
				       strerror(errno));
			goto cleanup;
		}

		/* A second trigger on the same fd must fail with EBUSY. */
		errno = 0;
		if (write_trigger(fd, "full 150000 2000000") != -1 || errno != EBUSY) {
			ksft_print_msg("second trigger: expected EBUSY, got %s\n",
				       strerror(errno));
			goto cleanup;
		}

		close(fd);
		fd = pressure_open(resources[i]);
		if (fd < 0)
			goto cleanup;

		/* window == 0 -> EINVAL */
		errno = 0;
		if (write_trigger(fd, "some 150000 0") != -1 || errno != EINVAL) {
			ksft_print_msg("window=0: expected EINVAL, got %s\n",
				       strerror(errno));
			goto cleanup;
		}
		/* window > 10s -> EINVAL */
		errno = 0;
		if (write_trigger(fd, "some 150000 20000000") != -1 || errno != EINVAL) {
			ksft_print_msg("window>10s: expected EINVAL, got %s\n",
				       strerror(errno));
			goto cleanup;
		}
		/* threshold == 0 -> EINVAL */
		errno = 0;
		if (write_trigger(fd, "some 0 2000000") != -1 || errno != EINVAL) {
			ksft_print_msg("threshold=0: expected EINVAL, got %s\n",
				       strerror(errno));
			goto cleanup;
		}
		/* threshold > window -> EINVAL */
		errno = 0;
		if (write_trigger(fd, "some 3000000 2000000") != -1 || errno != EINVAL) {
			ksft_print_msg("threshold>window: expected EINVAL, got %s\n",
				       strerror(errno));
			goto cleanup;
		}
		/* unparseable input -> EINVAL */
		errno = 0;
		if (write_trigger(fd, "foo 1 2") != -1 || errno != EINVAL) {
			ksft_print_msg("garbage: expected EINVAL, got %s\n",
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

/* /proc/pressure/irq is full-only: "some" fails EINVAL (skip if absent). */
static int test_proc_pressure_irq(const char *root)
{
	int ret = KSFT_FAIL;
	int fd;

	(void)root;

	fd = pressure_open("irq");
	if (fd < 0) {
		if (errno == ENOENT)
			return KSFT_SKIP;
		ksft_print_msg("open /proc/pressure/irq: %s\n", strerror(errno));
		return KSFT_FAIL;
	}

	errno = 0;
	if (write_trigger(fd, "some 150000 1000000") != -1 || errno != EINVAL) {
		ksft_print_msg("irq 'some' trigger: expected EINVAL, got %s\n",
			       strerror(errno));
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	close(fd);
	return ret;
}

/* cgroup.pressure 0/1 hides/shows *.pressure; ERANGE/EINVAL on bad input. */
static int test_cgroup_pressure_toggle(const char *root)
{
	char *cg = NULL;
	char buf[BUF_SIZE];
	int ret = KSFT_FAIL;

	cg = cg_name(root, "psi_toggle_test");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;

	/* Disable: reads back 0 and memory.pressure becomes unreadable. */
	if (cg_write(cg, "cgroup.pressure", "0")) {
		ksft_print_msg("failed to disable cgroup.pressure\n");
		goto cleanup;
	}
	if (cg_read(cg, "cgroup.pressure", buf, sizeof(buf)) < 0 || atoi(buf) != 0) {
		ksft_print_msg("cgroup.pressure=0 readback: '%s'\n", buf);
		goto cleanup;
	}
	if (cg_read(cg, "memory.pressure", buf, sizeof(buf)) >= 0) {
		ksft_print_msg("memory.pressure still visible while disabled\n");
		goto cleanup;
	}

	/* Re-enable: reads back 1 and memory.pressure is visible again. */
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

	/* Out-of-range -> ERANGE, non-numeric -> EINVAL. */
	if (cg_write(cg, "cgroup.pressure", "2") != -ERANGE) {
		ksft_print_msg("cgroup.pressure=2: expected ERANGE\n");
		goto cleanup;
	}
	if (cg_write(cg, "cgroup.pressure", "foo") != -EINVAL) {
		ksft_print_msg("cgroup.pressure=foo: expected EINVAL\n");
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	if (cg)
		cg_destroy(cg);
	free(cg);
	return ret;
}

/* Touch every page to force allocation under a tight memory.max. */
static void churn_memory(size_t size)
{
	char *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	int iter;

	if (p == MAP_FAILED)
		return;
	for (iter = 0; iter < 5; iter++) {
		size_t i;

		for (i = 0; i < size; i += 4096)
			p[i] = (char)(iter + i);
	}
	munmap(p, size);
}

/* Induce a stall under tight memory.max; expect poll() wake (SKIP on timeout). */
static int test_cgroup_trigger_fire(const char *root)
{
	char *cg = NULL, *mempress = NULL;
	int fd = -1, ret = KSFT_FAIL;
	struct pollfd pfd;
	pid_t pid;

	cg = cg_name(root, "psi_trigger_test");
	if (!cg)
		goto cleanup;
	if (cg_create(cg))
		goto cleanup;

	/* A tight memory.max forces direct reclaim while churning. */
	if (cg_write(cg, "memory.max", "50000000")) {
		ksft_print_msg("failed to set memory.max\n");
		goto cleanup;
	}
	if (cg_enter_current(cg)) {
		ksft_print_msg("failed to enter cgroup\n");
		goto cleanup;
	}

	mempress = cg_control(cg, "memory.pressure");
	if (!mempress)
		goto cleanup;
	fd = open(mempress, O_RDWR);
	if (fd < 0) {
		ksft_print_msg("open memory.pressure: %s\n", strerror(errno));
		goto cleanup;
	}

	/* 1us threshold in a 1s window: any memory stall fires it. */
	errno = 0;
	if (write_trigger(fd, "some 1 1000000") <= 0) {
		ksft_print_msg("arming trigger failed: %s\n", strerror(errno));
		goto cleanup;
	}

	pid = fork();
	if (pid < 0) {
		ksft_print_msg("fork: %s\n", strerror(errno));
		goto cleanup;
	}
	if (pid == 0) {
		churn_memory(PSI_CHURN_SIZE);
		_exit(0);
	}

	pfd.fd = fd;
	pfd.events = POLLPRI;
	switch (poll(&pfd, 1, PSI_POLL_TIMEOUT_MS)) {
	case -1:
		ksft_print_msg("poll: %s\n", strerror(errno));
		break;
	case 0:
		ksft_print_msg("no trigger event; could not induce memstall\n");
		ret = KSFT_SKIP;
		break;
	default:
		if (pfd.revents & (POLLPRI | POLLIN))
			ret = KSFT_PASS;
		else
			ksft_print_msg("poll returned 0x%x\n", pfd.revents);
		break;
	}

	waitpid(pid, NULL, 0);

cleanup:
	if (fd >= 0)
		close(fd);
	if (cg) {
		cg_enter_current(root);
		cg_destroy(cg);
	}
	free(mempress);
	free(cg);
	return ret;
}

#define TEST(x) { #x, x }

struct psi_test {
	const char *name;
	int (*fn)(const char *root);
};

static struct psi_test tests[] = {
	TEST(test_proc_pressure_validation),
	TEST(test_proc_pressure_irq),
	TEST(test_cgroup_pressure_toggle),
	TEST(test_cgroup_trigger_fire),
};

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	int i;

	(void)argc;

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(tests));

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");
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
