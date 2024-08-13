// SPDX-License-Identifier: GPL-2.0
#include "workingset_report.h"

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

#include "../clone3/clone3_selftests.h"

#define REFRESH_INTERVAL 5000
#define MB(x) (x << 20)

static void sleep_ms(int milliseconds)
{
	struct timespec ts;

	ts.tv_sec = milliseconds / 1000;
	ts.tv_nsec = (milliseconds % 1000) * 1000000;
	nanosleep(&ts, NULL);
}

/*
 * Checks if two given values differ by less than err% of their sum.
 */
static inline int values_close(long a, long b, int err)
{
	return labs(a - b) <= (a + b) / 100 * err;
}

static const char * const PAGE_AGE_INTERVALS[] = {
	"6000", "10000", "15000", "18446744073709551615",
};
#define NR_PAGE_AGE_INTERVALS (ARRAY_SIZE(PAGE_AGE_INTERVALS))

static int set_page_age_intervals_all_nodes(const char *intervals, int nr_nodes)
{
	int i;

	for (i = 0; i < nr_nodes; ++i) {
		int err = sysfs_set_page_age_intervals_str(
			i, &intervals[i * 1024], strlen(&intervals[i * 1024]));

		if (err < 0)
			return err;
	}
	return 0;
}

static int get_page_age_intervals_all_nodes(char *intervals, int nr_nodes)
{
	int i;

	for (i = 0; i < nr_nodes; ++i) {
		int err = sysfs_get_page_age_intervals_str(
			i, &intervals[i * 1024], 1024);

		if (err < 0)
			return err;
	}
	return 0;
}

static int set_refresh_interval_all_nodes(const long *interval, int nr_nodes)
{
	int i;

	for (i = 0; i < nr_nodes; ++i) {
		int err = sysfs_set_refresh_interval(i, interval[i]);

		if (err < 0)
			return err;
	}
	return 0;
}

static int get_refresh_interval_all_nodes(long *interval, int nr_nodes)
{
	int i;

	for (i = 0; i < nr_nodes; ++i) {
		long val = sysfs_get_refresh_interval(i);

		if (val < 0)
			return val;
		interval[i] = val;
	}
	return 0;
}

static pid_t clone_and_run(int fn(void *arg), void *arg)
{
	pid_t pid;

	struct __clone_args args = {
		.exit_signal = SIGCHLD,
	};

	pid = sys_clone3(&args, sizeof(struct __clone_args));

	if (pid == 0)
		exit(fn(arg));

	return pid;
}

static int read_workingset(int pagetype, int nid,
			   unsigned long page_age[NR_PAGE_AGE_INTERVALS])
{
	int i, err;
	char buf[4096];

	err = sysfs_page_age_read(nid, buf, sizeof(buf));
	if (err < 0)
		return err;

	for (i = 0; i < NR_PAGE_AGE_INTERVALS; ++i) {
		err = page_age_read(buf, PAGE_AGE_INTERVALS[i], pagetype);
		if (err < 0)
			return err;
		page_age[i] = err;
	}

	return 0;
}

static ssize_t read_interval_all_nodes(int pagetype, int interval)
{
	int i, err;
	unsigned long page_age[NR_PAGE_AGE_INTERVALS];
	ssize_t ret = 0;
	int nr_nodes = get_nr_nodes();

	for (i = 0; i < nr_nodes; ++i) {
		err = read_workingset(pagetype, i, page_age);
		if (err < 0)
			return err;

		ret += page_age[interval];
	}

	return ret;
}

#define TEST_SIZE MB(500l)

static int run_test(int f(void))
{
	int i, err, test_result;
	long *old_refresh_intervals;
	long *new_refresh_intervals;
	char *old_page_age_intervals;
	int nr_nodes = get_nr_nodes();

	if (nr_nodes <= 0) {
		ksft_print_msg("failed to get nr_nodes\n");
		return KSFT_FAIL;
	}

	old_refresh_intervals = calloc(nr_nodes, sizeof(long));
	new_refresh_intervals = calloc(nr_nodes, sizeof(long));
	old_page_age_intervals = calloc(nr_nodes, 1024);

	if (!(old_refresh_intervals && new_refresh_intervals &&
	      old_page_age_intervals)) {
		ksft_print_msg("failed to allocate memory for intervals\n");
		return KSFT_FAIL;
	}

	err = get_refresh_interval_all_nodes(old_refresh_intervals, nr_nodes);
	if (err < 0) {
		ksft_print_msg("failed to read refresh interval\n");
		return KSFT_FAIL;
	}

	err = get_page_age_intervals_all_nodes(old_page_age_intervals, nr_nodes);
	if (err < 0) {
		ksft_print_msg("failed to read page age interval\n");
		return KSFT_FAIL;
	}

	for (i = 0; i < nr_nodes; ++i)
		new_refresh_intervals[i] = REFRESH_INTERVAL;

	for (i = 0; i < nr_nodes; ++i) {
		err = sysfs_set_page_age_intervals(i, PAGE_AGE_INTERVALS,
						   NR_PAGE_AGE_INTERVALS - 1);
		if (err < 0) {
			ksft_print_msg("failed to set page age interval\n");
			test_result = KSFT_FAIL;
			goto fail;
		}
	}

	err = set_refresh_interval_all_nodes(new_refresh_intervals, nr_nodes);
	if (err < 0) {
		ksft_print_msg("failed to set refresh interval\n");
		test_result = KSFT_FAIL;
		goto fail;
	}

	sync();
	drop_pagecache();

	test_result = f();

fail:
	err = set_refresh_interval_all_nodes(old_refresh_intervals, nr_nodes);
	if (err < 0) {
		ksft_print_msg("failed to restore refresh interval\n");
		test_result = KSFT_FAIL;
	}
	err = set_page_age_intervals_all_nodes(old_page_age_intervals, nr_nodes);
	if (err < 0) {
		ksft_print_msg("failed to restore page age interval\n");
		test_result = KSFT_FAIL;
	}
	return test_result;
}

static char *file_test_path;
static int test_file(void)
{
	ssize_t ws_size_ref, ws_size_test;
	int ret = KSFT_FAIL, i;
	pid_t pid = 0;

	if (!file_test_path) {
		ksft_print_msg("Set a path to test file workingset\n");
		return KSFT_SKIP;
	}

	ws_size_ref = read_interval_all_nodes(PAGETYPE_FILE, 0);
	if (ws_size_ref < 0)
		goto cleanup;

	pid = clone_and_run(alloc_file_workingset, (void *)TEST_SIZE);
	if (pid < 0)
		goto cleanup;

	read_interval_all_nodes(PAGETYPE_FILE, 0);
	sleep_ms(REFRESH_INTERVAL);

	for (i = 0; i < 3; ++i) {
		sleep_ms(REFRESH_INTERVAL);
		ws_size_test = read_interval_all_nodes(PAGETYPE_FILE, 0);
		ws_size_test += read_interval_all_nodes(PAGETYPE_FILE, 1);
		if (ws_size_test < 0)
			goto cleanup;

		if (!values_close(ws_size_test - ws_size_ref, TEST_SIZE, 10)) {
			ksft_print_msg(
				"file working set size difference too large: actual=%ld, expected=%ld\n",
				ws_size_test - ws_size_ref, TEST_SIZE);
			goto cleanup;
		}
	}
	ret = KSFT_PASS;

cleanup:
	if (pid > 0)
		kill(pid, SIGKILL);
	cleanup_file_workingset();
	return ret;
}

static int test_anon(void)
{
	ssize_t ws_size_ref, ws_size_test;
	pid_t pid = 0;
	int ret = KSFT_FAIL, i;

	ws_size_ref = read_interval_all_nodes(PAGETYPE_ANON, 0);
	ws_size_ref += read_interval_all_nodes(PAGETYPE_ANON, 1);
	if (ws_size_ref < 0)
		goto cleanup;

	pid = clone_and_run(alloc_anon_workingset, (void *)TEST_SIZE);
	if (pid < 0)
		goto cleanup;

	sleep_ms(REFRESH_INTERVAL);
	read_interval_all_nodes(PAGETYPE_ANON, 0);

	for (i = 0; i < 5; ++i) {
		sleep_ms(REFRESH_INTERVAL);
		ws_size_test = read_interval_all_nodes(PAGETYPE_ANON, 0);
		ws_size_test += read_interval_all_nodes(PAGETYPE_ANON, 1);
		if (ws_size_test < 0)
			goto cleanup;

		if (!values_close(ws_size_test - ws_size_ref, TEST_SIZE, 10)) {
			ksft_print_msg(
				"anon working set size difference too large: actual=%ld, expected=%ld\n",
				ws_size_test - ws_size_ref, TEST_SIZE);
			goto cleanup;
		}
	}
	ret = KSFT_PASS;

cleanup:
	if (pid > 0)
		kill(pid, SIGKILL);
	return ret;
}


#define T(x) { x, #x }
struct workingset_test {
	int (*fn)(void);
	const char *name;
} tests[] = {
	T(test_anon),
	T(test_file),
};
#undef T

int main(int argc, char **argv)
{
	int i, err;

	if (argc > 1)
		file_test_path = argv[1];

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		err = run_test(tests[i].fn);
		ksft_test_result_code(err, tests[i].name, NULL);
	}
	return 0;
}
