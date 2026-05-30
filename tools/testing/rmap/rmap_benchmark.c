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
#include <stdbool.h>

int page_size;

/* Used for test_anon and test_file */
#define NR_SHARERS		256	/* Number of child processes sharing a single page */

/* KSM sysfs paths */
#define TEST_PATTERN            0xaa
#define KSM_RUN_PATH		"/sys/kernel/mm/ksm/run"
#define KSM_SLEEP_MS_PATH	"/sys/kernel/mm/ksm/sleep_millisecs"
#define KSM_PAGES_TO_SCAN	"/sys/kernel/mm/ksm/pages_to_scan"
#define KSM_FULL_SCANS_PATH	"/sys/kernel/mm/ksm/full_scans"
#define KSM_MAX_SHARING_PATH	"/sys/kernel/mm/ksm/max_page_sharing"

/* Tracepoint control paths - enable all events under rmap */
#define TRACE_ENABLE		"/sys/kernel/tracing/events/rmap/enable"
#define TRACE_FILE		"/sys/kernel/tracing/trace"

/*
 * Kernel TASK_COMM_LEN is 16. We use a slightly larger buffer
 * to safely accommodate the string plus null terminator without
 * depending on internal kernel headers.
 */
#define COMM_BUF_SIZE	32

#define MAX_TRACING_PENDING 128

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
static int write_sys(const char *path, int value)
{
	char buf[32];
	int fd;
	ssize_t ret;

	snprintf(buf, sizeof(buf), "%d", value);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
		return -1;
	}
	ret = write(fd, buf, strlen(buf));
	close(fd);
	if (ret != (ssize_t)strlen(buf)) {
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
	if (write_sys(KSM_RUN_PATH, 1) < 0) {
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

static void disable_tracepoint(void)
{
	write_sys(TRACE_ENABLE, 0);
}

/* Tracepoint enable/disable */
static int enable_tracepoint(void)
{
	struct stat st;
	int fd;

	if (stat("/sys/kernel/tracing/trace", &st) != 0) {
		if (mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, NULL) != 0)
			fprintf(stderr, "Warning: mount tracefs failed: %s\n", strerror(errno));
	}

	if (write_sys(TRACE_ENABLE, 1) < 0)
		return -1;

	fd = open(TRACE_FILE, O_WRONLY | O_TRUNC);
	if (fd < 0) {
		perror("open " TRACE_FILE);
		disable_tracepoint();
		return -1;
	}
	close(fd);

	return 0;
}

/*
 * Get current process comm (task_struct->comm).
 * Returns 0 on success, -1 on failure.
 */
static int get_self_comm(char *buf, size_t size)
{
	FILE *fp = fopen("/proc/self/comm", "r");
	size_t len;

	if (!fp)
		return -1;

	if (!fgets(buf, size, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* Strip trailing newline */
	len = strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	return 0;
}

/* Timestamp extraction (us) */
static unsigned long long extract_timestamp_us(const char *line)
{
	char time_str[32];
	double ts_sec = 0.0;

	if (sscanf(line, "%*s %*s %*s %31s", time_str) == 1) {
		char *colon = strchr(time_str, ':');

		if (colon)
			*colon = '\0';
		ts_sec = strtod(time_str, NULL);
	}
	return (unsigned long long)(ts_sec * 1e6);
}

/* Safe start/end pairing using folio and rwc addresses, plus PID filter */
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
	struct pending_start pending[MAX_TRACING_PENDING];
	int pending_cnt = 0;
	unsigned long long sum = 0, max_val = 0;
	int pairs = 0;
	const char *type_str = page_type_str(type);
	char type_pattern[64];
	char my_comm[COMM_BUF_SIZE];
	bool overflow_warned = false;
	/* COMM(32) + '-' + PID(max 7 digits), using 16 for pid is safe */
	char match_pattern[COMM_BUF_SIZE + 16];

	if (get_self_comm(my_comm, sizeof(my_comm)) < 0) {
		fprintf(stderr, "Failed to read /proc/self/comm\n");
		fclose(fp);
		return -1;
	}

	snprintf(type_pattern, sizeof(type_pattern), "page_type=%s", type_str);
	/* Get current PID for filtering */
	pid_t mypid = getpid();

	snprintf(match_pattern, sizeof(match_pattern), "%s-%d ", my_comm, mypid);

	while (fgets(line, sizeof(line), fp)) {
		/* Filter by COMM-PID: line should start with "COMM-PID" */
		if (!strstr(line, match_pattern))
			continue;
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
			if (pending_cnt < MAX_TRACING_PENDING) {
				pending[pending_cnt].ts = extract_timestamp_us(line);
				pending[pending_cnt].folio = folio;
				pending[pending_cnt].rwc = rwc;
				pending_cnt++;
			} else if (!overflow_warned) {
				fprintf(stderr, "Warning: pending_start overflow, some events may be lost\n");
				overflow_warned = true; /* Only warn once */
			}
		} else if (strstr(line, "rmap_walk_end:")) {
			unsigned long long end_ts = extract_timestamp_us(line);
			/* Find matching start event */
			for (int i = 0; i < pending_cnt; i++) {
				if (pending[i].folio == folio && pending[i].rwc == rwc) {
					unsigned long long delta = end_ts - pending[i].ts;

					if (delta > max_val)
						max_val = delta;
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

/*
 * Trigger rmap_walk by moving a single page.
 * Returns 0 on success, -1 on failure (e.g., single NUMA node).
 */
static int trigger_rmap_walk(void *region)
{
	int ret, status, cur_node, target_node;
	void *pages[1];
	int nodes[1];

	ret = move_pages(0, 1, (void **)&region, NULL, &status, MPOL_MF_MOVE_ALL);
	if (ret != 0) {
		perror("Failed to get original numa");
		return -1;
	}
	cur_node = status;

	for (target_node = 0; target_node <= numa_max_node(); target_node++) {
		if (numa_bitmask_isbitset(numa_all_nodes_ptr, target_node) && target_node != cur_node)
			break;
	}
	if (target_node > numa_max_node()) {
		fprintf(stderr, "No other NUMA node available, cannot trigger migration.\n");
		return -1;
	}

	pages[0] = region;
	nodes[0] = target_node;
	ret = move_pages(0, 1, pages, nodes, &status, MPOL_MF_MOVE_ALL);
	if (ret < 0) {
		perror("move_pages");
		return -1;
	}
	return 0;
}

/*
 * Fork nr_forks child processes sharing the same memory region (COW not broken).
 * The parent and all children will have a VMA mapping the same physical page.
 *
 * On success, returns nr_forks and stores the child PIDs in *out_pids.
 * The caller is responsible for freeing *out_pids and killing/waiting children.
 * On failure, returns -1 and cleans up any partially forked children internally.
 */
static int fork_sharers(int nr_forks, pid_t **out_pids)
{
	pid_t *pids = malloc(sizeof(pid_t) * nr_forks);

	if (!pids) {
		perror("malloc pids");
		return -1;
	}

	int i, forked = 0;

	for (i = 0; i < nr_forks; i++) {
		pid_t pid = fork();

		if (pid == 0) {
			/* Child: just wait for signal to exit */
			free(pids);
			pause();
			exit(0);
		} else if (pid > 0) {
			pids[i] = pid;
			forked++;
		} else {
			perror("fork");
			break;
		}
	}

	if (forked < nr_forks) {
		/* Fork failed: kill already forked children */
		for (int j = 0; j < forked; j++) {
			kill(pids[j], SIGTERM);
			waitpid(pids[j], NULL, 0);
		}
		free(pids);
		return -1;
	}

	/* Give children a moment to settle */
	usleep(100000);

	*out_pids = pids;
	return forked;
}

/* Helper: kill and reap all child sharers */
static void cleanup_children(pid_t *pids, int nr_children)
{
	for (int i = 0; i < nr_children; i++) {
		kill(pids[i], SIGTERM);
		waitpid(pids[i], NULL, 0);
	}
	free(pids);
}


/*
 * Split VMA with mprotect (used only for KSM test).
 * Returns number of successful mprotects (or -1 on error).
 */
static int split_vma_with_mprotect(void *addr, size_t size)
{
	int splits = 0;
	size_t pages = size / page_size;

	for (size_t i = 0; i < pages; i++) {
		if (i % 2 == 0) {
			if (mprotect(addr + i * page_size, page_size, PROT_READ) < 0) {
				perror("mprotect");
				return -1;
			}
			splits++;
		}
	}

	return splits;
}

/* KSM configuration save/restore */
static struct ksm_config {
	int run;
	int sleep_ms;
	int pages_to_scan;
	int max_page_sharing;
} orig_ksm;

static int save_ksm_config(void)
{
	if (read_sys_int(KSM_RUN_PATH, &orig_ksm.run) ||
	    read_sys_int(KSM_SLEEP_MS_PATH, &orig_ksm.sleep_ms) ||
	    read_sys_int(KSM_PAGES_TO_SCAN, &orig_ksm.pages_to_scan) ||
	    read_sys_int(KSM_MAX_SHARING_PATH, &orig_ksm.max_page_sharing)) {
		fprintf(stderr, "Failed to read KSM config\n");
		return -1;
	}
	return 0;
}

static void restore_ksm_config(void)
{
	write_sys(KSM_RUN_PATH, orig_ksm.run);
	write_sys(KSM_SLEEP_MS_PATH, orig_ksm.sleep_ms);
	write_sys(KSM_PAGES_TO_SCAN, orig_ksm.pages_to_scan);
	write_sys(KSM_MAX_SHARING_PATH, orig_ksm.max_page_sharing);
}

/*
 * KSM test (shared by many VMAs via mprotect splitting).
 * the total memory area is 20000 pages, and split into 20000 VMAs.
 * Restricted by KSM config (max_page_sharing = 256), so any one of
 * KSM pages is shared by 256 VMAs at maximum.
 */
static void test_ksm(void)
{
	int nr_ksm_pages = 20000;
	int config_max_page_sharing = 256;
	size_t size = nr_ksm_pages * page_size;
	unsigned long long max_us, avg_us;
	int count;

	if (save_ksm_config() < 0) {
		printf("KSM not available, skip KSM test.\n");
		return;
	}

	if (write_sys(KSM_RUN_PATH, 2) < 0 ||
	    write_sys(KSM_SLEEP_MS_PATH, 0) < 0 ||
	    write_sys(KSM_MAX_SHARING_PATH, config_max_page_sharing) < 0 ||
	    write_sys(KSM_PAGES_TO_SCAN, 10000) < 0) {
		fprintf(stderr, "Failed to configure KSM\n");
		goto restore_out;
	}

	void *region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (region == MAP_FAILED) {
		perror("mmap for KSM");
		goto restore_out;
	}

	memset(region, TEST_PATTERN, size);
	if (madvise(region, size, MADV_MERGEABLE) != 0) {
		perror("madvise MADV_MERGEABLE");
		goto unmap_out;
	}

	if (write_sys(KSM_RUN_PATH, 1) < 0) {
		perror("Start KSM");
		goto unmap_out;
	}

	if (split_vma_with_mprotect(region, size) == -1)
		goto unmap_out;

	wait_ksm_merge();

	if (enable_tracepoint() != 0)
		goto unmap_out;

	if (trigger_rmap_walk(region + page_size) != 0) {
		disable_tracepoint();
		goto unmap_out;
	}
	usleep(100000);
	disable_tracepoint();

	if (parse_trace_and_print(PAGE_TYPE_KSM, &max_us, &avg_us, &count) == 0) {
		printf("KSM rmap_walk latency (Shared by %d VMAs via mprotect and KSM merge):\n",
			config_max_page_sharing);
		printf("  Max: %.2f ms (%.0f us)\n", max_us/1000.0, (double)max_us);
		printf("  Avg: %.2f ms (%.0f us)\n", avg_us/1000.0, (double)avg_us);
		printf("  Count: %d events\n", count);
	}
unmap_out:
	munmap(region, size);
restore_out:
	restore_ksm_config();
}

/* Anonymous test: fork many child processes to share a single physical page */
static void test_anon(void)
{
	unsigned long long max_us, avg_us;
	int count;
	int nr_forks = NR_SHARERS - 1;
	pid_t *pids = NULL;

	void *region = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap anon for sharing");
		return;
	}
	memset(region, TEST_PATTERN, page_size);

	int forked = fork_sharers(nr_forks, &pids);

	if (forked < 0) {
		printf("Failed to fork enough children, abort anonymous test.\n");
		goto munmap_out;
	}

	if (enable_tracepoint() != 0)
		goto cleanup_child_out;

	if (trigger_rmap_walk(region) != 0) {
		disable_tracepoint();
		goto cleanup_child_out;
	}
	usleep(100000);
	disable_tracepoint();

	if (parse_trace_and_print(PAGE_TYPE_ANON, &max_us, &avg_us, &count) == 0) {
		printf("Anonymous page rmap_walk latency (Shared by %d VMAs via fork, COW not broken):\n", forked + 1);
		printf("  Max: %.2f ms (%.0f us)\n", max_us / 1000.0, (double)max_us);
		printf("  Avg: %.2f ms (%.0f us)\n", avg_us / 1000.0, (double)avg_us);
		printf("  Count: %d events\n", count);
	}

cleanup_child_out:
	cleanup_children(pids, forked);
munmap_out:
	munmap(region, page_size);
}

/* File-backed test: similar to anonymous but using MAP_SHARED file mapping */
static void test_file(void)
{
	unsigned long long max_us, avg_us;
	int count;
	int nr_forks = NR_SHARERS - 1;
	pid_t *pids = NULL;
	char filename[] = "/tmp/rmap_test_file_XXXXXX";
	int fd = mkstemp(filename);

	if (fd < 0) {
		perror("mkstemp");
		return;
	}
	unlink(filename);
	if (ftruncate(fd, page_size) < 0) {
		perror("ftruncate");
		goto close_fd_out;
	}

	void *region = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (region == MAP_FAILED) {
		perror("mmap file for sharing");
		goto close_fd_out;
	}
	memset(region, TEST_PATTERN, page_size);

	int forked = fork_sharers(nr_forks, &pids);

	if (forked < 0) {
		printf("Failed to fork enough children, abort file test.\n");
		goto munmap_out;
	}

	if (enable_tracepoint() != 0)
		goto cleanup_child_out;

	if (trigger_rmap_walk(region) != 0) {
		disable_tracepoint();
		goto cleanup_child_out;
	}

	usleep(100000);
	disable_tracepoint();

	if (parse_trace_and_print(PAGE_TYPE_FILE, &max_us, &avg_us, &count) == 0) {
		printf("File page rmap_walk latency (Shared by %d VMAs via fork, MAP_SHARED):\n", forked + 1);
		printf("  Max: %.2f ms (%.0f us)\n", max_us / 1000.0, (double)max_us);
		printf("  Avg: %.2f ms (%.0f us)\n", avg_us / 1000.0, (double)avg_us);
		printf("  Count: %d events\n", count);
	}

cleanup_child_out:
	cleanup_children(pids, forked);
munmap_out:
	munmap(region, page_size);
close_fd_out:
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

