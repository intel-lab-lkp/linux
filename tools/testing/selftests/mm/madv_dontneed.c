// SPDX-License-Identifier: GPL-2.0-only
/*
 * MADV_DONTNEED and PROCESS_MADV_DONTNEED tests
 *
 * Copyright (C) 2025, Linx Software Corp.
 *
 * Author(s): Lian Wang <lianux.mm@gmail.com>
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include "vm_util.h"
#include <time.h>

#include "../kselftest.h"

/*
 * For now, we're using 2 MiB of private anonymous memory for all tests.
 */
#define SIZE (256 * 1024 * 1024)

static size_t pagesize;

static void sense_support(void)
{
	char *addr;
	int ret;

	addr = mmap(0, pagesize, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	if (!addr)
		ksft_exit_fail_msg("mmap failed\n");

	ret = madvise(addr, pagesize, MADV_DONTNEED);
	if (ret)
		ksft_exit_skip("MADV_DONTNEED is not available\n");

	munmap(addr, pagesize);
}

/*
 * Read pagemap to check page is present in mermory
 */
static bool is_page_present(void *addr)
{
	uintptr_t vaddr = (uintptr_t)addr;
	uintptr_t offset = (vaddr / pagesize) * sizeof(uint64_t);
	ssize_t bytes_read;
	uint64_t entry;
	bool ret;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		ksft_exit_fail_msg("opening pagemap failed\n");
		ret = false;
	}

	if ((lseek(fd, offset, SEEK_SET)) == -1) {
		close(fd);
		ret = false;
	}

	bytes_read = read(fd, &entry, sizeof(entry));
	close(fd);

	if (bytes_read != sizeof(entry)) {
		perror("read failed");
		return false;
	}

	if (entry & (1ULL << 63))
		ret = true;

	return ret;
}

/*
 * test madvsise_dontneed
 */
static void test_madv_dontneed(void)
{
	unsigned long rss_anon_before, rss_anon_after;
	bool present, rss;
	char *addr;
	int ret;

	ksft_print_msg("[RUN] %s\n", __func__);

	addr = mmap(0, SIZE, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	if (!addr)
		ksft_exit_fail_msg("mmap failed\n");

	memset(addr, 0x7A, SIZE);

	rss_anon_before = rss_anon();
	if (!rss_anon_before)
		ksft_exit_fail_msg("No RssAnon is allocated before dontneed\n");
	ret = madvise(addr, SIZE, MADV_DONTNEED);
	ksft_test_result(!ret, "MADV_DONTNEED\n");

	rss_anon_after = rss_anon();
	if (rss_anon_after < rss_anon_before)
		rss = true;
	ksft_test_result(rss, "MADV_DONTNEED rss is correct\n");

	for (size_t i = 0; i < SIZE; i += pagesize) {
		present = is_page_present(addr + i);
		if (present) {
			ksft_print_msg("Page not zero at offset %zu\n",
				       (size_t)i);
		}
	}

	ksft_test_result(!present, "MADV_DONTNEED page is present\n");
	munmap(addr, SIZE);
}

/*
 * Measure performance of batched process_madvise vs madvise
 */
static int measure_process_madvise_batching(int hint, int total_size,
					    int single_unit, int batch_size)
{
	struct iovec *vec = malloc(sizeof(*vec) * batch_size);
	struct timespec start, end;
	unsigned long elapsed_ns = 0;
	unsigned long nr_measures = 0;
	pid_t pid = getpid();
	char *buf;
	int pidfd;

	pidfd = syscall(SYS_pidfd_open, pid, 0);
	if (pidfd == -1) {
		perror("pidfd_open fail");
		return -1;
	}

	buf = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		perror("mmap fail");
		goto out;
	}

	if (!vec) {
		perror("malloc vec failed");
		goto unmap_out;
	}

	while (elapsed_ns < 5UL * 1000 * 1000 * 1000) {
		memset(buf, 0x7A, total_size);

		clock_gettime(CLOCK_MONOTONIC, &start);

		for (int off = 0; off < total_size;
		     off += single_unit * batch_size) {
			for (int i = 0; i < batch_size; i++) {
				vec[i].iov_base = buf + off + i * single_unit;
				vec[i].iov_len = single_unit;
			}
			syscall(SYS_process_madvise, pidfd, vec, batch_size,
				hint, 0);
		}

		clock_gettime(CLOCK_MONOTONIC, &end);
		elapsed_ns += (end.tv_sec - start.tv_sec) * 1e9 +
			      (end.tv_nsec - start.tv_nsec);
		nr_measures++;
	}

	ksft_print_msg("[RESULT] batch=%d time=%.3f us/op\n", batch_size,
		       (double)(elapsed_ns / nr_measures) /
			       (total_size / single_unit));

	free(vec);
unmap_out:
	munmap(buf, total_size);
out:
	close(pidfd);
	return 0;
}

static void test_perf_batch_process(void)
{
	ksft_print_msg("[RUN] %s\n", __func__);
	measure_process_madvise_batching(MADV_DONTNEED, SIZE, pagesize, 1);
	measure_process_madvise_batching(MADV_DONTNEED, SIZE, pagesize, 2);
	measure_process_madvise_batching(MADV_DONTNEED, SIZE, pagesize, 4);
	ksft_test_result(1, "All test were done\n");
}

int main(int argc, char **argv)
{
	int err;

	pagesize = getpagesize();

	ksft_print_header();
	ksft_set_plan(4);

	sense_support();
	test_madv_dontneed();
	test_perf_batch_process();

	err = ksft_get_fail_cnt();
	if (err)
		ksft_exit_fail_msg("%d out of %d tests failed\n", err,
				   ksft_test_num());
	ksft_exit_pass();
}
