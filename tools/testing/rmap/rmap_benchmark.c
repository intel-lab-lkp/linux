// SPDX-License-Identifier: GPL-2.0
/*
 * Reverse mapping latency test for KSM, anonymous and file pages
 *
 * This program creates a large number of pages (KSM merged, normal anonymous,
 * or file mapped), splits the VMA into many small VMAs via mprotect,
 * triggers rmap_walk by move_pages(), and collects latency data from the
 * tracepoints 'rmap_walk_start' and 'rmap_walk_end' (offline timestamp diff).
 *
 * Usage: must be run as root (to access tracefs and KSM sysfs).
 *
 * Copyright 2026, ZTE Corp.
 *
 * Author(s): Xu Xin <xu.xin16@zte.com.cn>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <numaif.h>
#include <numa.h>
#include <time.h>
#include <ctype.h>

/* Page size and test parameters */
int page_size;
#define NR_PAGES		20000	/* Number of virtual pages */
#define TEST_PATTERN		0xaa

/* KSM sysfs paths */
#define KSM_RUN_PATH		"/sys/kernel/mm/ksm/run"
#define KSM_SLEEP_MS_PATH	"/sys/kernel/mm/ksm/sleep_millisecs"
#define KSM_PAGES_TO_SCAN	"/sys/kernel/mm/ksm/pages_to_scan"
#define KSM_FULL_SCANS_PATH	"/sys/kernel/mm/ksm/full_scans"

/* Tracepoint control paths - enable all events under rmap */
#define TRACE_ENABLE		"/sys/kernel/tracing/events/rmap/enable"
#define TRACE_FILE		"/sys/kernel/tracing/trace"

/*
 * Page types for rmap_walk tracepoint filtering
 */
enum page_type {
	PAGE_TYPE_KSM,
	PAGE_TYPE_ANON,
	PAGE_TYPE_FILE,
};

static const char *page_type_str(enum page_type type)
{
	switch (type) {
	case PAGE_TYPE_KSM:	return "ksm";
	case PAGE_TYPE_ANON:	return "anon";
	case PAGE_TYPE_FILE:	return "file";
	default:		return "unknown";
	}
}

/*
 * Write a string to a sysfs file.
 */
static int write_sys(const char *path, const char *value)
{
	int fd;
	ssize_t ret;

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	ret = write(fd, value, strlen(value));
	close(fd);
	if (ret != (ssize_t)strlen(value)) {
		fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

/*
 * Read an integer from a sysfs file.
 */
static int read_sys_int(const char *path)
{
	FILE *fp;
	int val;

	fp = fopen(path, "r");
	if (!fp) {
		fprintf(stderr, "fopen %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	if (fscanf(fp, "%d", &val) != 1) {
		fprintf(stderr, "fscanf %s failed\n", path);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return val;
}

/*
 * Get KSM full scan count.
 */
static int ksm_get_full_scans(void)
{
	return read_sys_int(KSM_FULL_SCANS_PATH);
}

/*
 * Wait for KSM to complete at least two full scans, which ensures that
 * merging has had a chance to happen.
 */
static void wait_ksm_merge(void)
{
	int start_scans, end_scans;
	int max_wait = 60;
	int waited = 0;

	start_scans = ksm_get_full_scans();
	if (start_scans < 0) {
		fprintf(stderr, "Failed to read initial full_scans\n");
		return;
	}

	/* Make sure KSM is running */
	if (write_sys(KSM_RUN_PATH, "1") < 0) {
		fprintf(stderr, "Failed to start KSM\n");
		return;
	}

	do {
		sleep(1);
		end_scans = ksm_get_full_scans();
		if (end_scans < 0) {
			fprintf(stderr, "Failed to read full_scans\n");
			return;
		}
		waited++;
		if (waited > max_wait) {
			fprintf(stderr, "Warning: KSM full_scans not increased "
				"after %d seconds\n", max_wait);
			break;
		}
	} while (end_scans < start_scans + 2);
}

/*
 * Enable the rmap tracepoints and clear the trace buffer.
 */
static void enable_tracepoint(void)
{
	int fd;
	struct stat st;

	/* Check if tracefs is already accessible */
	if (stat("/sys/kernel/tracing/trace", &st) != 0) {
		/* Try to mount tracefs */
		if (mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL) != 0) {
			fprintf(stderr, "Warning: Failed to mount tracefs: %s\n",
				strerror(errno));
			/* Continue anyway, maybe it's already mounted elsewhere */
		}
	}

	if (write_sys(TRACE_ENABLE, "1") < 0)
		exit(1);
	/* Truncate the trace file to clear old data */
	fd = open(TRACE_FILE, O_WRONLY | O_TRUNC);
	if (fd < 0) {
		perror("open " TRACE_FILE);
		exit(1);
	}
	close(fd);
}

/*
 * Disable the rmap tracepoints.
 */
static void disable_tracepoint(void)
{
	write_sys(TRACE_ENABLE, "0");
}

/*
 * Extract timestamp from a trace line (the 5th field, before colon).
 * Returns timestamp in microseconds.
 */
static unsigned long long extract_timestamp_us(const char *line)
{
	char time_str[32];
	double ts_sec = 0.0;

	/* %*s skip first field (task-name), second field (PID in brackets? Actually "[CPU]"?
	 * The format is: <COMM>-<PID> [CPU] FLAGS TIMESTAMP: ...
	 * Example: "rmap_benchmark_-467     [003] ..... 17258.057760: rmap_walk_start: ..."
	 * Split by whitespace:
	 * 0: "rmap_benchmark_-467"
	 * 1: "[003]"
	 * 2: "....."
	 * 3: "17258.057760:"  <-- timestamp with colon
	 * Therefore timestamp is the 4th field (index 3). But careful: there might be extra spaces.
	 * So we need to skip three fields: %*s %*s %*s, then read the timestamp string.
	 */
	if (sscanf(line, "%*s %*s %*s %31s", time_str) == 1) {
		/* Remove trailing colon if present */
		char *colon = strchr(time_str, ':');

		if (colon)
			*colon = '\0';
		ts_sec = strtod(time_str, NULL);
	}
	return (unsigned long long)(ts_sec * 1e6);
}

/*
 * Parse the trace file and compute latency from start/end events for a given page_type.
 * Returns 0 on success, -1 if no matching events.
 */
static int parse_trace_and_print(enum page_type type, unsigned long long *max_us,
				 unsigned long long *avg_us, int *count)
{
	FILE *fp;
	char line[1024];
	unsigned long long start_ts = 0;
	unsigned long long delta;
	unsigned long long sum = 0, max_val = 0;
	int pairs = 0;
	const char *type_str = page_type_str(type);
	char type_pattern[64];

	snprintf(type_pattern, sizeof(type_pattern), "page_type=%s", type_str);

	fp = fopen(TRACE_FILE, "r");
	if (!fp) {
		perror("fopen " TRACE_FILE);
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		/* Check if line contains our page_type */
		if (!strstr(line, type_pattern))
			continue;

		if (strstr(line, "rmap_walk_start:")) {
			start_ts = extract_timestamp_us(line);
		} else if (strstr(line, "rmap_walk_end:")) {
			if (start_ts) {
				unsigned long long end_ts = extract_timestamp_us(line);

				if (end_ts > start_ts) {
					delta = end_ts - start_ts;
					sum += delta;
					if (delta > max_val)
						max_val = delta;
					pairs++;
				}
				start_ts = 0; /* reset after pairing */
			}
		}
	}
	fclose(fp);

	if (pairs == 0) {
		printf("No rmap_walk events with page_type=%s found.\n", type_str);
		return -1;
	}

	*max_us = max_val;
	*avg_us = sum / pairs;
	*count = pairs;
	return 0;
}

/*
 * Trigger rmap_walk by moving a single page.
 * region: pointer to the page (any page in the mapped region).
 * The function will try to move that page to a different NUMA node.
 */
static void trigger_rmap_walk(void *region)
{
	int ret, status, cur_node, target_node;
	void *pages[1];
	int nodes[1];

	printf("Triggering rmap_walk via move_pages...\n");

	ret = move_pages(0, 1, (void **)&region, NULL, &status, MPOL_MF_MOVE_ALL);
	if (ret != 0) {
		perror("Failed to get original numa");
		exit(1);
	}
	cur_node = status;

	for (target_node = 0; target_node <= numa_max_node(); target_node++) {
		if (numa_bitmask_isbitset(numa_all_nodes_ptr, target_node) &&
		    target_node != cur_node)
			break;
	}
	if (target_node > numa_max_node()) {
		printf("Couldn't find available numa node for testing\n");
		exit(1);
	}

	pages[0] = region;
	nodes[0] = target_node;

	/*
	 * Note: We ignore the return value when ret >= 0, since there's probability
	 * that ksmd's ksm_get_folio collides with do_move_page(), which cause
	 * __migrate_folio failed due to the check "folio_ref_count(src) !=
	 * expected_count".
	 */
	ret = move_pages(0, 1, pages, nodes, &status, MPOL_MF_MOVE_ALL);
	if (ret < 0)
		perror("move_pages");
}

/*
 * Split a VMA into many small VMAs by changing protection on every other page.
 * This increases the number of anon_vma_chain entries and makes rmap_walk slower.
 */
static void split_vma_with_mprotect(void *addr, size_t size)
{
	for (size_t i = 0; i < size / page_size; i++) {
		if (i % 2 == 0) {
			if (mprotect(addr + i * page_size, page_size, PROT_READ) < 0) {
				if (errno != EACCES)
					perror("mprotect");
			}
		}
	}
}

/*
 * Test for KSM pages.
 */
static void test_ksm(void)
{
	void *region;
	size_t size = NR_PAGES * page_size;
	unsigned long long max_us, avg_us;
	int count;

	printf("\n=== Testing KSM pages ===\n");

	/* Stop KSM and set aggressive scan parameters */
	if (write_sys(KSM_RUN_PATH, "2") < 0) {
		printf("Warning: CONFIG_KSM might be not build, skip testing.\n");
		return;
	}
	if (write_sys(KSM_SLEEP_MS_PATH, "0") < 0 ||
	    write_sys(KSM_PAGES_TO_SCAN, "10000") < 0)
		exit(1);

	region = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap for KSM");
		exit(1);
	}
	memset(region, TEST_PATTERN, size);

	if (madvise(region, size, MADV_MERGEABLE) != 0) {
		perror("madvise MADV_MERGEABLE");
		munmap(region, size);
		exit(1);
	}

	/* Start KSM scanner */
	if (write_sys(KSM_RUN_PATH, "1") < 0) {
		munmap(region, size);
		exit(1);
	}

	split_vma_with_mprotect(region, size);

	/* Wait full merging */
	wait_ksm_merge();

	enable_tracepoint();
	/* Move the page at offset page_size (any page is fine) */
	trigger_rmap_walk(region + page_size);
	usleep(100000);		/* allow trace to be written */
	disable_tracepoint();

	if (parse_trace_and_print(PAGE_TYPE_KSM, &max_us, &avg_us, &count) == 0) {
		printf("KSM rmap_walk latency:\n");
		printf("  Maximum duration: %.2f ms (%.0f us)\n",
		       max_us / 1000.0, (double)max_us);
		printf("  Average duration: %.2f ms (%.0f us)\n",
		       avg_us / 1000.0, (double)avg_us);
		printf("  Count: %d events\n", count);
	}

	munmap(region, size);
	write_sys(KSM_RUN_PATH, "2");	/* stop KSM */
}

/*
 * Test for normal anonymous pages.
 */
static void test_anon(void)
{
	void *region;
	size_t size = NR_PAGES * page_size;
	unsigned long long max_us, avg_us;
	int count;

	printf("\n=== Testing anonymous pages ===\n");

	region = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap for anonymous");
		exit(1);
	}
	memset(region, TEST_PATTERN, size);

	split_vma_with_mprotect(region, size);

	enable_tracepoint();
	trigger_rmap_walk(region + page_size);
	usleep(100000);
	disable_tracepoint();

	if (parse_trace_and_print(PAGE_TYPE_ANON, &max_us, &avg_us, &count) == 0) {
		printf("Anonymous page rmap_walk latency:\n");
		printf("  Maximum duration: %.2f ms (%.0f us)\n",
		       max_us / 1000.0, (double)max_us);
		printf("  Average duration: %.2f ms (%.0f us)\n",
		       avg_us / 1000.0, (double)avg_us);
		printf("  Count: %d events\n", count);
	}

	munmap(region, size);
}

/*
 * Test for file-backed pages (mmap of a temporary file).
 */
static void test_file(void)
{
	void *region;
	size_t size = NR_PAGES * page_size;
	int fd;
	char filename[] = "/tmp/rmap_test_file_XXXXXX";

	printf("\n=== Testing file pages ===\n");

	fd = mkstemp(filename);
	if (fd < 0) {
		perror("mkstemp");
		exit(1);
	}
	if (ftruncate(fd, size) < 0) {
		perror("ftruncate");
		unlink(filename);
		close(fd);
		exit(1);
	}

	region = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, 0);
	if (region == MAP_FAILED) {
		perror("mmap for file");
		unlink(filename);
		close(fd);
		exit(1);
	}
	memset(region, TEST_PATTERN, size);

	split_vma_with_mprotect(region, size);

	enable_tracepoint();
	trigger_rmap_walk(region + page_size);
	usleep(100000);
	disable_tracepoint();

	unsigned long long max_us, avg_us;
	int count;

	if (parse_trace_and_print(PAGE_TYPE_FILE, &max_us, &avg_us, &count) == 0) {
		printf("File page rmap_walk latency:\n");
		printf("  Maximum duration: %.2f ms (%.0f us)\n",
		       max_us / 1000.0, (double)max_us);
		printf("  Average duration: %.2f ms (%.0f us)\n",
		       avg_us / 1000.0, (double)avg_us);
		printf("  Count: %d events\n", count);
	}

	munmap(region, size);
	unlink(filename);
	close(fd);
}

int main(void)
{
	page_size = getpagesize();

	/* Need root for tracefs and KSM sysfs */
	if (geteuid() != 0) {
		fprintf(stderr, "This program must be run as root.\n");
		exit(1);
	}

	if (numa_available() < 0) {
		printf("Warning: NUMA not available, move_pages may not work.\n");
		exit(1);
	}

	/* Run three tests */
	test_ksm();
	test_anon();
	test_file();

	return 0;
}
