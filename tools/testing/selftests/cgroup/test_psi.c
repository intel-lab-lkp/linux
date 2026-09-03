// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <linux/limits.h>

#include "../kselftest_harness.h"
#include "cgroup_util.h"

#define PSI_POLL_TIMEOUT_MS	5000

/*
 * The kernel parser overwrites the last byte of a trigger write with a
 * NUL, so terminate with a newline to keep the payload intact.
 */
static ssize_t write_trigger(int fd, const char *trigger)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%s\n", trigger);

	return write(fd, buf, strlen(buf));
}

static int pressure_open(const char *resource)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/proc/pressure/%s", resource);
	return open(path, O_RDWR);
}

FIXTURE(psi)
{
	char root[PATH_MAX];
	char *cg;
};

FIXTURE_SETUP(psi)
{
	int psi_fd;

	if (cg_find_unified_root(self->root, sizeof(self->root), NULL))
		SKIP(return, "cgroup v2 isn't mounted");

	/* PSI must be enabled (CONFIG_PSI=y, not disabled on the cmdline). */
	psi_fd = open("/proc/pressure/memory", O_RDONLY);
	if (psi_fd < 0)
		SKIP(return, "PSI unavailable (CONFIG_PSI=n or psi=0)");
	close(psi_fd);

	self->cg = cg_name(self->root, "psi_trigger_test");
	ASSERT_NE(NULL, self->cg);
	ASSERT_EQ(0, cg_create(self->cg));
}

FIXTURE_TEARDOWN(psi)
{
	cg_killall(self->cg);
	cg_destroy(self->cg);
	free(self->cg);
}

/*
 * /proc/pressure/<resource> accepts exactly one trigger per file
 * descriptor. Verify that a "some" trigger arms and that a second
 * trigger on the same fd is rejected with EBUSY.
 */
static void test_psi_write(struct __test_metadata *_metadata,
			   const char *filename)
{
	int fd;

	fd = pressure_open(filename);
	ASSERT_GE(fd, 0);
	ASSERT_GT(write_trigger(fd, "some 150000 2000000"), 0);
	ASSERT_EQ(-1, write_trigger(fd, "full 150000 2000000"));
	ASSERT_EQ(EBUSY, errno);
	close(fd);
}

TEST_F(psi, proc_trigger_io)
{
	test_psi_write(_metadata, "io");
}

TEST_F(psi, proc_trigger_memory)
{
	test_psi_write(_metadata, "memory");
}

TEST_F(psi, proc_trigger_cpu)
{
	test_psi_write(_metadata, "cpu");
}

/*
 * irq only tracks "full", so a "some" trigger must be rejected while a
 * "full" trigger arms. irq is optional -- it only exists with IRQ-time
 * accounting -- so a missing /proc/pressure/irq is SKIP, not FAIL.
 */
TEST_F(psi, proc_trigger_irq)
{
	int fd;

	fd = pressure_open("irq");
	if (fd < 0)
		SKIP(return, "/proc/pressure/irq unavailable");

	ASSERT_EQ(-1, write_trigger(fd, "some 150000 2000000"));
	ASSERT_GT(write_trigger(fd, "full 150000 2000000"), 0);
	close(fd);
}

/*
 * cgroup.pressure gates visibility of the per-resource *.pressure files
 * inside a cgroup: writing 0 hides them, writing 1 shows them again.
 * Drive one hide/show cycle and check that memory.pressure appears and
 * disappears along with it.
 */
TEST_F(psi, cgroup_pressure_toggle)
{
	char buf[BUF_SIZE];

	ASSERT_EQ(0, cg_write(self->cg, "cgroup.pressure", "0"));
	ASSERT_EQ(0, cg_read_strcmp(self->cg, "cgroup.pressure", "0\n"));
	ASSERT_LT(cg_read(self->cg, "memory.pressure", buf, sizeof(buf)), 0);

	ASSERT_EQ(0, cg_write(self->cg, "cgroup.pressure", "1"));
	ASSERT_EQ(0, cg_read_strcmp(self->cg, "cgroup.pressure", "1\n"));
	ASSERT_GE(cg_read(self->cg, "memory.pressure", buf, sizeof(buf)), 0);
}

/*
 * A child that burns CPU forever; stopped by cg_killall() on teardown.
 * It also dies with the runner, so an interrupted run (e.g. Ctrl-C
 * during poll()) does not leave orphaned hogs pinning every CPU.
 */
static int hog_cpu(const char *cgroup, void *arg)
{
	prctl(PR_SET_PDEATHSIG, SIGKILL);
	for (;;) {}
	return 0;
}

/*
 * Arm a "some" trigger on a cgroup's cpu.pressure, oversubscribe the
 * cgroup with more spinning hogs than there are CPUs, and check that the
 * trigger fires once the cgroup stalls on CPU.
 */
TEST_F(psi, cgroup_trigger_fire)
{
	char *cpupress;
	struct pollfd pfd = { .events = POLLPRI };
	long ncpus;
	int fd;
	int i;

	cpupress = cg_control(self->cg, "cpu.pressure");
	ASSERT_NE(NULL, cpupress);
	fd = open(cpupress, O_RDWR);
	free(cpupress);
	ASSERT_GE(fd, 0);
	pfd.fd = fd;

	/* 1usec threshold over a 2s window: any CPU stall fires it. */
	ASSERT_GT(write_trigger(fd, "some 1 2000000"), 0);

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	ASSERT_NE(-1, ncpus);

	/* ncpus+1 hogs guarantee CPU contention inside the cgroup. */
	for (i = 0; i < ncpus + 1; i++)
		ASSERT_GE(cg_run_nowait(self->cg, hog_cpu, NULL), 0);

	ASSERT_EQ(1, poll(&pfd, 1, PSI_POLL_TIMEOUT_MS));
	ASSERT_NE(0, pfd.revents & POLLPRI);
	close(fd);
}

/*
 * The flip side of cgroup_trigger_fire: with the threshold equal to the
 * whole window, even ncpus+1 hogs must not accumulate enough stall to
 * fire the trigger.
 */
TEST_F(psi, cgroup_trigger_no_fire)
{
	char *cpupress;
	struct pollfd pfd = { .events = POLLPRI };
	long ncpus;
	int fd;
	int i;

	cpupress = cg_control(self->cg, "cpu.pressure");
	ASSERT_NE(NULL, cpupress);
	fd = open(cpupress, O_RDWR);
	free(cpupress);
	ASSERT_GE(fd, 0);
	pfd.fd = fd;

	ASSERT_GT(write_trigger(fd, "some 2000000 2000000"), 0);

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	ASSERT_NE(-1, ncpus);

	for (i = 0; i < ncpus + 1; i++)
		ASSERT_GE(cg_run_nowait(self->cg, hog_cpu, NULL), 0);

	ASSERT_EQ(0, poll(&pfd, 1, PSI_POLL_TIMEOUT_MS));
	close(fd);
}

TEST_HARNESS_MAIN
