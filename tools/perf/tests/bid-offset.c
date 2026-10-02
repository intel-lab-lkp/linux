// SPDX-License-Identifier: GPL-2.0
#include "tests.h"
#include "debug.h"
#include "evlist.h"
#include "evsel.h"
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include "../perf-sys.h"

static int test__bid_offset(struct test_suite *test __maybe_unused, int subtest __maybe_unused)
{
	struct perf_event_attr attr = {
		.type		= PERF_TYPE_SOFTWARE,
		.config		= PERF_COUNT_SW_DUMMY,
		.size		= sizeof(attr),
		.sample_type	= PERF_SAMPLE_BUILD_ID_OFFSET,
		.exclude_kernel = 1,
		.exclude_hv     = 1,
	};
	int fd;

	fd = sys_perf_event_open(&attr, /*pid=*/0, /*cpu=*/-1, /*group_fd=*/-1, /*flags=*/0);
	if (fd < 0) {
		pr_debug("sys_perf_event_open failed with %d\n", errno);
		if (errno == EINVAL || errno == EACCES || errno == EPERM) {
			pr_debug("Kernel does not support or permit PERF_SAMPLE_BUILD_ID_OFFSET\n");
			return TEST_SKIP;
		}
		return TEST_FAIL;
	}
	close(fd);

	attr.sample_type = PERF_SAMPLE_CALLCHAIN_BUILD_ID_OFFSET;
	fd = sys_perf_event_open(&attr, /*pid=*/0, /*cpu=*/-1, /*group_fd=*/-1, /*flags=*/0);
	if (fd < 0) {
		pr_debug("sys_perf_event_open with callchain failed with %d\n", errno);
		if (errno == EINVAL || errno == EACCES || errno == EPERM) {
			pr_debug("Kernel does not support or permit PERF_SAMPLE_CALLCHAIN_BUILD_ID_OFFSET\n");
			return TEST_SKIP;
		}
		return TEST_FAIL;
	}
	close(fd);

	return TEST_OK;
}

static struct test_case bid_offset_tests[] = {
	TEST_CASE_REASON("Test PERF_SAMPLE_BUILD_ID_OFFSET", bid_offset,
			 "not supported by kernel"),
	{ .name = NULL, }
};

struct test_suite suite__bid_offset = {
	.desc = "Test PERF_SAMPLE_BUILD_ID_OFFSET",
	.test_cases = bid_offset_tests,
};
