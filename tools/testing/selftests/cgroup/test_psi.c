// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/limits.h>

#include "kselftest.h"
#include "cgroup_util.h"

#define PSI_POLL_TIMEOUT_MS	5000

/* PSI triggers are written with a trailing NUL the kernel parser expects. */
static ssize_t write_trigger(int fd, const char *trigger)
{
	return write(fd, trigger, strlen(trigger) + 1);
}

static int pressure_open(const char *resource)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
	fd = open(path, O_RDWR);
	if (fd < 0)
		ksft_print_msg("open %s: %s\n", path, strerror(errno));
	return fd;
}

/*
 * /proc/pressure/<resource> accepts exactly one trigger per file
 * descriptor. For io, memory and cpu verify that a "some" trigger arms
 * and that a second trigger on the same fd is rejected with EBUSY. For
 * irq, which only tracks "full", verify that "some" is rejected and
 * "full" arms. irq is optional -- it only exists with IRQ-time
 * accounting -- so a missing /proc/pressure/irq is SKIP, not FAIL.
 */
static int test_proc_trigger(const char *resource, bool full_only)
{
	int fd, ret = KSFT_FAIL;

	fd = pressure_open(resource);
	if (fd < 0)
		return full_only ? KSFT_SKIP : KSFT_FAIL;

	if (!full_only) {
		if (write_trigger(fd, "some 150000 2000000") <= 0) {
			ksft_print_msg("%s: 'some' trigger rejected: %s\n",
				       resource, strerror(errno));
			goto out;
		}
		if (write_trigger(fd, "full 150000 2000000") != -1 ||
		    errno != EBUSY) {
			ksft_print_msg("%s: second trigger not EBUSY\n",
				       resource);
			goto out;
		}
	} else {
		if (write_trigger(fd, "some 150000 2000000") != -1) {
			ksft_print_msg("irq: 'some' trigger unexpectedly accepted\n");
			goto out;
		}
		if (write_trigger(fd, "full 150000 2000000") <= 0) {
			ksft_print_msg("irq: 'full' trigger rejected: %s\n",
				       strerror(errno));
			goto out;
		}
	}

	ret = KSFT_PASS;
out:
	close(fd);
	return ret;
}

/*
 * cgroup.pressure gates visibility of the per-resource *.pressure files
 * inside a cgroup: writing 0 hides them, writing 1 shows them again.
 * Drive one hide/show cycle and check that memory.pressure appears and
 * disappears along with it.
 */
static int test_cgroup_pressure_toggle(const char *root)
{
	char buf[BUF_SIZE];
	char *cg = NULL;
	int ret = KSFT_FAIL, created = 0;

	cg = cg_name(root, "psi_toggle_test");
	if (!cg)
		goto cleanup;
	if (cg_create(cg)) {
		ksft_print_msg("cg_create: %s\n", strerror(errno));
		goto cleanup;
	}
	created = 1;

	if (cg_write(cg, "cgroup.pressure", "0")) {
		ksft_print_msg("write cgroup.pressure=0: %s\n", strerror(errno));
		goto cleanup;
	}
	if (cg_read_strcmp(cg, "cgroup.pressure", "0\n")) {
		ksft_print_msg("cgroup.pressure readback != 0\n");
		goto cleanup;
	}
	if (cg_read(cg, "memory.pressure", buf, sizeof(buf)) >= 0) {
		ksft_print_msg("memory.pressure readable while hidden\n");
		goto cleanup;
	}

	if (cg_write(cg, "cgroup.pressure", "1")) {
		ksft_print_msg("write cgroup.pressure=1: %s\n", strerror(errno));
		goto cleanup;
	}
	if (cg_read_strcmp(cg, "cgroup.pressure", "1\n")) {
		ksft_print_msg("cgroup.pressure readback != 1\n");
		goto cleanup;
	}
	if (cg_read(cg, "memory.pressure", buf, sizeof(buf)) < 0) {
		ksft_print_msg("memory.pressure unreadable after enabling\n");
		goto cleanup;
	}

	ret = KSFT_PASS;
cleanup:
	if (created)
		cg_destroy(cg);
	free(cg);
	return ret;
}

/* A child that burns CPU forever; stopped by cg_killall() in the parent. */
static int hog_cpu(const char *cgroup, void *arg)
{
	for (;;)
		;
	return 0;
}

/*
 * Arm a "some" trigger on a cgroup's cpu.pressure, oversubscribe the
 * cgroup with more spinning hogs than there are CPUs, and check that the
 * trigger fires once the cgroup stalls on CPU.
 */
static int test_cgroup_trigger_fire(const char *root)
{
	char *cg = NULL, *cpupress = NULL;
	int fd = -1, ret = KSFT_FAIL, created = 0, i;
	long ncpus;

	cg = cg_name(root, "psi_trigger_test");
	if (!cg)
		goto cleanup;
	if (cg_create(cg)) {
		ksft_print_msg("cg_create: %s\n", strerror(errno));
		goto cleanup;
	}
	created = 1;

	cpupress = cg_control(cg, "cpu.pressure");
	if (!cpupress)
		goto cleanup;
	fd = open(cpupress, O_RDWR);
	if (fd < 0) {
		ksft_print_msg("open cpu.pressure: %s\n", strerror(errno));
		goto cleanup;
	}

	/*
	 * 1usec threshold over a 2s window: any CPU stall fires it. The 2s
	 * window is the smallest unprivileged users are allowed to arm.
	 */
	if (write_trigger(fd, "some 1 2000000") <= 0) {
		ksft_print_msg("arm trigger: %s\n", strerror(errno));
		goto cleanup;
	}

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus <= 0) {
		ksft_print_msg("sysconf(_SC_NPROCESSORS_ONLN) returned %ld\n",
			       ncpus);
		goto cleanup;
	}

	/* ncpus+1 hogs guarantee CPU contention inside the cgroup. */
	for (i = 0; i < ncpus + 1; i++) {
		if (cg_run_nowait(cg, hog_cpu, NULL) < 0) {
			ksft_print_msg("spawn hog %d: %s\n", i, strerror(errno));
			goto cleanup;
		}
	}

	struct pollfd pfd = { .fd = fd, .events = POLLPRI };

	switch (poll(&pfd, 1, PSI_POLL_TIMEOUT_MS)) {
	case -1:
		ksft_print_msg("poll: %s\n", strerror(errno));
		goto cleanup;
	case 0:
		ksft_print_msg("trigger did not fire (could not induce CPU pressure)\n");
		ret = KSFT_SKIP;
		break;
	default:
		if (pfd.revents & POLLPRI)
			ret = KSFT_PASS;
		else
			ksft_print_msg("poll returned 0x%x\n", pfd.revents);
		break;
	}

cleanup:
	if (fd >= 0)
		close(fd);
	if (created) {
		cg_killall(cg);
		cg_destroy(cg);
	}
	free(cpupress);
	free(cg);
	return ret;
}

struct psi_proc_test {
	const char *name;
	const char *resource;
	bool full_only;
};
static const struct psi_proc_test proc_tests[] = {
	{ "proc_trigger_io",     "io",     false },
	{ "proc_trigger_memory", "memory", false },
	{ "proc_trigger_cpu",    "cpu",    false },
	{ "proc_trigger_irq",    "irq",    true  },
};

struct psi_cg_test {
	const char *name;
	int (*fn)(const char *root);
};
static const struct psi_cg_test cg_tests[] = {
	{ "cgroup_pressure_toggle", test_cgroup_pressure_toggle },
	{ "cgroup_trigger_fire",    test_cgroup_trigger_fire },
};

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	int psi_fd, i;

	ksft_print_header();
	ksft_set_plan(ARRAY_SIZE(proc_tests) + ARRAY_SIZE(cg_tests));

	if (cg_find_unified_root(root, sizeof(root), NULL))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	/* PSI must be enabled (CONFIG_PSI=y, not disabled on the cmdline). */
	psi_fd = open("/proc/pressure/memory", O_RDONLY);
	if (psi_fd < 0)
		ksft_exit_skip("PSI unavailable (CONFIG_PSI=n or psi=0)\n");
	close(psi_fd);

	for (i = 0; i < ARRAY_SIZE(proc_tests); i++) {
		switch (test_proc_trigger(proc_tests[i].resource,
					  proc_tests[i].full_only)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", proc_tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", proc_tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", proc_tests[i].name);
			break;
		}
	}

	for (i = 0; i < ARRAY_SIZE(cg_tests); i++) {
		switch (cg_tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", cg_tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", cg_tests[i].name);
			break;
		default:
			ksft_test_result_fail("%s\n", cg_tests[i].name);
			break;
		}
	}

	ksft_finished();
}
