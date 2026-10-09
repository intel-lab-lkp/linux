// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <linux/fs.h>
#include <linux/loop.h>

#include "kselftest_harness.h"

#define DEV_SIZE	(4 * 1024 * 1024)
#define SOFT_BLKSIZE	1024U	/* < PAGE_SIZE: several buffer_heads/folio */

#define STRESS_DURATION_SEC	30
#define STRESS_MAX_CHUNK_BLKS	8

FIXTURE(blkdev_writeback) {
	int fd_backing;
	int fd_dev;
	char dev_path[32];
};

static void fill_pattern(char *buf, size_t len, unsigned seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (char)(seed + i);
}

static void write_pattern(struct __test_metadata *_metadata,
			  FIXTURE_DATA(blkdev_writeback) *self,
			  off_t offset, size_t len, unsigned seed)
{
	char *buf = malloc(len);

	ASSERT_NE(buf, NULL);
	fill_pattern(buf, len, seed);
	ASSERT_EQ(pwrite(self->fd_dev, buf, len, offset), (ssize_t)len);
	free(buf);
}

static void verify_pattern(struct __test_metadata *_metadata,
			   FIXTURE_DATA(blkdev_writeback) *self,
			   off_t offset, size_t len, unsigned seed)
{
	char *expected = malloc(len);
	char *actual = malloc(len);
	size_t i;

	ASSERT_NE(expected, NULL);
	ASSERT_NE(actual, NULL);
	fill_pattern(expected, len, seed);

	ASSERT_EQ(fsync(self->fd_dev), 0);
	ASSERT_EQ(pread(self->fd_backing, actual, len, offset), (ssize_t)len);

	for (i = 0; i < len; i++)
		ASSERT_EQ(actual[i], expected[i])
			TH_LOG("mismatch at backing file offset %zu",
			       (size_t)offset + i);

	free(expected);
	free(actual);
}

static void verify_zero(struct __test_metadata *_metadata,
			FIXTURE_DATA(blkdev_writeback) *self,
			off_t offset, size_t len)
{
	char *actual = malloc(len);
	size_t i;

	ASSERT_NE(actual, NULL);
	ASSERT_EQ(pread(self->fd_backing, actual, len, offset), (ssize_t)len);

	for (i = 0; i < len; i++)
		ASSERT_EQ(actual[i], 0)
			TH_LOG("unexpected write at backing file offset %zu",
			       (size_t)offset + i);

	free(actual);
}

FIXTURE_SETUP(blkdev_writeback)
{
	int fd_ctl, loop_nr, blksize = SOFT_BLKSIZE;
	char template[] = "/tmp/blkdev_writeback_test_XXXXXX";
	struct loop_config config = { 0 };

	self->fd_backing = -1;
	self->fd_dev = -1;

	if (geteuid() != 0)
		SKIP(return, "Test requires root to attach a loop device");

	self->fd_backing = mkstemp(template);
	ASSERT_GE(self->fd_backing, 0);
	ASSERT_EQ(unlink(template), 0);
	ASSERT_EQ(ftruncate(self->fd_backing, DEV_SIZE), 0);

	fd_ctl = open("/dev/loop-control", O_RDWR);
	if (fd_ctl < 0)
		SKIP(return, "Loop device support is not available");
	loop_nr = ioctl(fd_ctl, LOOP_CTL_GET_FREE);
	ASSERT_GE(loop_nr, 0);
	ASSERT_EQ(close(fd_ctl), 0);

	snprintf(self->dev_path, sizeof(self->dev_path), "/dev/loop%d", loop_nr);
	self->fd_dev = open(self->dev_path, O_RDWR);
	ASSERT_GE(self->fd_dev, 0);

	config.fd = self->fd_backing;
	ASSERT_EQ(ioctl(self->fd_dev, LOOP_CONFIGURE, &config), 0);
	ASSERT_EQ(ioctl(self->fd_dev, BLKBSZSET, &blksize), 0);
}

FIXTURE_TEARDOWN(blkdev_writeback)
{
	if (self->fd_dev >= 0) {
		fsync(self->fd_dev);
		ioctl(self->fd_dev, LOOP_CLR_FD);
		close(self->fd_dev);
	}
	if (self->fd_backing >= 0)
		close(self->fd_backing);
}

/*
 * A single write spanning many contiguous folios should still read back
 * correctly regardless of how many bios blkdev_writepages() coalesced it
 * into.
 */
TEST_F(blkdev_writeback, contiguous_multi_folio)
{
	write_pattern(_metadata, self, 0, 512 * 1024, 0);
	verify_pattern(_metadata, self, 0, 512 * 1024, 0);
}

/*
 * Two writes separated by an untouched gap must each land correctly,this means the
 * non-contiguous block run must close out the current bio/run rather than merge across
 * the hole.
 */
TEST_F(blkdev_writeback, gapped_regions)
{
	const off_t region_a = 0;
	const off_t region_b = 192 * 1024;
	const size_t region_len = 64 * 1024;
	const off_t gap = 64 * 1024;
	const size_t gap_len = region_b - gap;

	write_pattern(_metadata, self, region_a, region_len, 0xa5);
	write_pattern(_metadata, self, region_b, region_len, 0x3c);

	verify_pattern(_metadata, self, region_a, region_len, 0xa5);
	verify_pattern(_metadata, self, region_b, region_len, 0x3c);
	verify_zero(_metadata, self, gap, gap_len);
}

/*
 * Many small, mutually non-adjacent writes force repeated run/bio splits
 * in __bh_bio_append(), stressing the tracking of buffer runs within a
 * coalesced bio.
 */
TEST_F(blkdev_writeback, scattered_writes)
{
	const size_t chunk_len = 4096;
	const unsigned nr_chunks = 32;
	const size_t stride = DEV_SIZE / nr_chunks;
	unsigned i;

	for (i = 0; i < nr_chunks; i++) {
		/* write in reverse order so nothing is naturally contiguous */
		unsigned idx = nr_chunks - 1 - i;

		write_pattern(_metadata, self, idx * stride, chunk_len, idx);
	}

	for (i = 0; i < nr_chunks; i++)
		verify_pattern(_metadata, self, i * stride, chunk_len, i);
}

static void stress_writer(FIXTURE_DATA(blkdev_writeback) *self)
{
	char buf[STRESS_MAX_CHUNK_BLKS * SOFT_BLKSIZE];
	unsigned seed = getpid();

	srand(seed);
	for (;;) {
		size_t blks = 1 + rand_r(&seed) % STRESS_MAX_CHUNK_BLKS;
		size_t len = blks * SOFT_BLKSIZE;
		size_t max_off_blks = (DEV_SIZE - len) / SOFT_BLKSIZE;
		off_t off = (rand_r(&seed) % max_off_blks) * SOFT_BLKSIZE;

		fill_pattern(buf, len, seed);
		if (pwrite(self->fd_dev, buf, len, off) != (ssize_t)len)
			exit(1);
	}
}

static void stress_syncer(FIXTURE_DATA(blkdev_writeback) *self)
{
	for (;;) {
		if (fsync(self->fd_dev) < 0)
			exit(1);
	}
}

/*
 * Concurrent writers dirtying overlapping, adjacent and disjoint ranges
 * while syncers repeatedly force writing the bio. Data content is not checked
 * (writes overlap by design), the point is to survive the duration without a
 * kernel-side warning or crash.
 */
TEST_F_TIMEOUT(blkdev_writeback, stress_concurrent_writeback, STRESS_DURATION_SEC + 30)
{
	long nr_procs = sysconf(_SC_NPROCESSORS_ONLN);
	pid_t pids[2 * nr_procs];
	int i;

	for (i = 0; i < nr_procs; i++) {
		pids[i] = fork();
		ASSERT_GE(pids[i], 0);
		if (pids[i] == 0)
			stress_writer(self);
	}
	for (i = 0; i < nr_procs; i++) {
		pids[nr_procs + i] = fork();
		ASSERT_GE(pids[nr_procs + i], 0);
		if (pids[nr_procs + i] == 0)
			stress_syncer(self);
	}

	ASSERT_EQ(clock_nanosleep(CLOCK_MONOTONIC, 0,
			&(struct timespec){ .tv_sec = STRESS_DURATION_SEC }, NULL), 0);

	for (i = 0; i < 2 * nr_procs; i++)
		kill(pids[i], SIGKILL);

	for (i = 0; i < 2 * nr_procs; i++) {
		int wstatus;

		ASSERT_EQ(waitpid(pids[i], &wstatus, 0), pids[i]);
		/* an exit before we killed it means it hit a real error */
		ASSERT_FALSE(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0);
	}
}

TEST_HARNESS_MAIN
