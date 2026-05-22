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

/* Helper: read/write sysfs */
static int write_sys(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	ssize_t ret = write(fd, value, strlen(value));
	close(fd);
	if (ret != (ssize_t)strlen(value)) {
		fprintf(stderr, "write %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	return 0;
}

static int read_sys_int(const char *path, int *val)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return -1;
	if (fscanf(fp, "%d", val) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

/* KSM full scan count */
static int ksm_get_full_scans(void)
{
	int val;
	if (read_sys_int(KSM_FULL_SCANS_PATH, &val))
		return -1;
	return val;
}

/* Wait for KSM full scans */
static void wait_ksm_merge(void)
{
	int start_scans, end_scans;
	int max_wait = 60, waited = 0;

	start_scans = ksm_get_full_scans();
	if (start_scans < 0) {
		fprintf(stderr, "Failed to read initial full_scans\n");
		return;
	}
	if (write_sys(KSM_RUN_PATH, "1") < 0) {
		fprintf(stderr, "Failed to start KSM\n");
		return;
	}
	do {
		sleep(1);
		end_scans = ksm_get_full_scans();
		if (end_scans < 0)
			return;
		waited++;
		if (waited > max_wait) {
			fprintf(stderr, "Warning: KSM full_scans not increased after %ds\n", max_wait);
			break;
		}
	} while (end_scans < start_scans + 2);
}

/* Tracepoint enable/disable */
static void enable_tracepoint(void)
{
	struct stat st;
	if (stat("/sys/kernel/tracing/trace", &st) != 0) {
		if (mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL) != 0)
			fprintf(stderr, "Warning: mount tracefs failed: %s\n", strerror(errno));
	}
	if (write_sys(TRACE_ENABLE, "1") < 0)
		exit(1);
	int fd = open(TRACE_FILE, O_WRONLY | O_TRUNC);
	if (fd < 0) {
		perror("open " TRACE_FILE);
		exit(1);
	}
	close(fd);
}

static void disable_tracepoint(void)
{
	write_sys(TRACE_ENABLE, "0");
}

/* Timestamp extraction (us) */
static unsigned long long extract_timestamp_us(const char *line)
{
	char time_str[32];
	double ts_sec = 0.0;
	if (sscanf(line, "%*s %*s %*s %31s", time_str) == 1) {
		char *colon = strchr(time_str, ':');
		if (colon) *colon = '\0';
		ts_sec = strtod(time_str, NULL);
	}
	return (unsigned long long)(ts_sec * 1e6);
}

/* Safe start/end pairing using folio and rwc addresses */
struct pending_start {
	unsigned long long ts;
	unsigned long folio;
	unsigned long rwc;
};

static int parse_trace_and_print(enum page_type type, unsigned long long *max_us,
				 unsigned long long *avg_us, int *count)
{
	FILE *fp = fopen(TRACE_FILE, "r");
	if (!fp) {
		perror("fopen " TRACE_FILE);
		return -1;
	}

	char line[1024];
	struct pending_start pending[128];
	int pending_cnt = 0;
	unsigned long long sum = 0, max_val = 0;
	int pairs = 0;
	const char *type_str = page_type_str(type);
	char type_pattern[64];
	snprintf(type_pattern, sizeof(type_pattern), "page_type=%s", type_str);

	while (fgets(line, sizeof(line), fp)) {
		if (!strstr(line, type_pattern))
			continue;

		/* Extract folio and rwc addresses */
		unsigned long folio = 0, rwc = 0;
		char *folio_str = strstr(line, "folio=");
		char *rwc_str = strstr(line, "rwc=");
		if (folio_str && rwc_str) {
			folio = strtoul(folio_str + 6, NULL, 16);
			rwc = strtoul(rwc_str + 4, NULL, 16);
		} else {
			continue;
		}

		if (strstr(line, "rmap_walk_start:")) {
			if (pending_cnt < 128) {
				pending[pending_cnt].ts = extract_timestamp_us(line);
				pending[pending_cnt].folio = folio;
				pending[pending_cnt].rwc = rwc;
				pending_cnt++;
			}
		} else if (strstr(line, "rmap_walk_end:")) {
			unsigned long long end_ts = extract_timestamp_us(line);
			/* Find matching start event */
			for (int i = 0; i < pending_cnt; i++) {
				if (pending[i].folio == folio && pending[i].rwc == rwc) {
					unsigned long long delta = end_ts - pending[i].ts;
					if (delta > max_val) max_val = delta;
					sum += delta;
					pairs++;
					/* Remove this pending entry */
					pending[i] = pending[--pending_cnt];
					break;
				}
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

/* Trigger rmap_walk via move_pages */
static void trigger_rmap_walk(void *region)
{
	int ret, status, cur_node, target_node;
	void *pages[1];
	int nodes[1];

	ret = move_pages(0, 1, (void **)&region, NULL, &status, MPOL_MF_MOVE_ALL);
	if (ret != 0) {
		perror("Failed to get original numa");
		exit(1);
	}
	cur_node = status;

	for (target_node = 0; target_node <= numa_max_node(); target_node++) {
		if (numa_bitmask_isbitset(numa_all_nodes_ptr, target_node) && target_node != cur_node)
			break;
	}
	if (target_node > numa_max_node()) {
		fprintf(stderr, "No other NUMA node\n");
		exit(1);
	}

	pages[0] = region;
	nodes[0] = target_node;
	ret = move_pages(0, 1, pages, nodes, &status, MPOL_MF_MOVE_ALL);
	if (ret < 0)
		perror("move_pages");
}

/* Split VMA with mprotect */
static void split_vma_with_mprotect(void *addr, size_t size)
{
	for (size_t i = 0; i < size / page_size; i++) {
		if (i % 2 == 0) {
			if (mprotect(addr + i * page_size, page_size, PROT_READ) < 0 && errno != EACCES)
				perror("mprotect");
		}
	}
}

/* KSM configuration save/restore */
static struct ksm_config {
	int run;
	int sleep_ms;
	int pages_to_scan;
} orig_ksm;

static int save_ksm_config(void)
{
	if (read_sys_int(KSM_RUN_PATH, &orig_ksm.run) ||
	    read_sys_int(KSM_SLEEP_MS_PATH, &orig_ksm.sleep_ms) ||
	    read_sys_int(KSM_PAGES_TO_SCAN, &orig_ksm.pages_to_scan)) {
		fprintf(stderr, "Failed to read KSM config\n");
		return -1;
	}
	return 0;
}

static void restore_ksm_config(void)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%d", orig_ksm.run);
	write_sys(KSM_RUN_PATH, buf);
	snprintf(buf, sizeof(buf), "%d", orig_ksm.sleep_ms);
	write_sys(KSM_SLEEP_MS_PATH, buf);
	snprintf(buf, sizeof(buf), "%d", orig_ksm.pages_to_scan);
	write_sys(KSM_PAGES_TO_SCAN, buf);
}

/* KSM test */
static void test_ksm(void)
{
	size_t size = NR_PAGES * page_size;
	unsigned long long max_us, avg_us;
	int count;

	if (save_ksm_config() < 0) {
		printf("KSM not available, skip KSM test.\n");
		return;
	}

	if (write_sys(KSM_RUN_PATH, "2") < 0 ||
	    write_sys(KSM_SLEEP_MS_PATH, "0") < 0 ||
	    write_sys(KSM_PAGES_TO_SCAN, "10000") < 0) {
		fprintf(stderr, "Failed to configure KSM\n");
		restore_ksm_config();
		return;
	}

	void *region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap for KSM");
		restore_ksm_config();
		return;
	}

	memset(region, TEST_PATTERN, size);
	if (madvise(region, size, MADV_MERGEABLE) != 0) {
		perror("madvise MADV_MERGEABLE");
		munmap(region, size);
		restore_ksm_config();
		return;
	}

	if (write_sys(KSM_RUN_PATH, "1") < 0) {
		perror("Start KSM");
		munmap(region, size);
		restore_ksm_config();
		return;
	}

	/* Construct a anon_vma shared by a number of unrelated VMAs */
	split_vma_with_mprotect(region, size);
	wait_ksm_merge();

	/* Trigger one page to be rmapped */
	enable_tracepoint();
	trigger_rmap_walk(region + page_size);
	usleep(100000);
	disable_tracepoint();

	if (parse_trace_and_print(PAGE_TYPE_KSM, &max_us, &avg_us, &count) == 0) {
		printf("KSM rmap_walk latency:\n");
		printf("  Max: %.2f ms (%.0f us)\n", max_us/1000.0, (double)max_us);
		printf("  Avg: %.2f ms (%.0f us)\n", avg_us/1000.0, (double)avg_us);
		printf("  Count: %d\n", count);
	}
	munmap(region, size);
	restore_ksm_config();
}

/* Anonymous test */
static void test_anon(void)
{
	size_t size = NR_PAGES * page_size;
	unsigned long long max_us, avg_us;
	int count;
	void *region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap anon");
		return;
	}
	memset(region, TEST_PATTERN, size);
	split_vma_with_mprotect(region, size);
	enable_tracepoint();
	trigger_rmap_walk(region + page_size);
	usleep(100000);
	disable_tracepoint();
	if (parse_trace_and_print(PAGE_TYPE_ANON, &max_us, &avg_us, &count) == 0) {
		printf("Anonymous page rmap_walk latency:\n");
		printf("  Max: %.2f ms (%.0f us)\n", max_us/1000.0, (double)max_us);
		printf("  Avg: %.2f ms (%.0f us)\n", avg_us/1000.0, (double)avg_us);
		printf("  Count: %d\n", count);
	}
	munmap(region, size);
}

/* File-backed test (with early unlink) */
static void test_file(void)
{
	size_t size = NR_PAGES * page_size;
	char filename[] = "/tmp/rmap_test_file_XXXXXX";
	int fd = mkstemp(filename);
	if (fd < 0) {
		perror("mkstemp");
		return;
	}
	unlink(filename);  /* file will vanish when fd closed, even on crash */
	if (ftruncate(fd, size) < 0) {
		perror("ftruncate");
		close(fd);
		return;
	}
	void *region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (region == MAP_FAILED) {
		perror("mmap file");
		close(fd);
		return;
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
		printf("  Max: %.2f ms (%.0f us)\n", max_us/1000.0, (double)max_us);
		printf("  Avg: %.2f ms (%.0f us)\n", avg_us/1000.0, (double)avg_us);
		printf("  Count: %d\n", count);
	}
	munmap(region, size);
	close(fd);
}

int main(void)
{
	page_size = getpagesize();

	if (geteuid() != 0) {
		fprintf(stderr, "Must be run as root.\n");
		return 1;
	}
	if (numa_available() < 0) {
		fprintf(stderr, "NUMA not available.\n");
		return 1;
	}

	test_ksm();
	test_anon();
	test_file();
	return 0;
}
