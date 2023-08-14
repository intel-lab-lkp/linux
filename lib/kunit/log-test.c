// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit test for logging.
 *
 * Based on code:
 * Copyright (C) 2019, Google LLC.
 * Author: Brendan Higgins <brendanhiggins@google.com>
 */
#include <kunit/test.h>

#include "string-stream.h"

static void kunit_log_test(struct kunit *test)
{
	struct kunit_suite suite;
	char *full_log;

	suite.log = alloc_string_stream(test, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, suite.log);
	string_stream_set_append_newlines(suite.log, true);

	kunit_log(KERN_INFO, test, "put this in log.");
	kunit_log(KERN_INFO, test, "this too.");
	kunit_log(KERN_INFO, &suite, "add to suite log.");
	kunit_log(KERN_INFO, &suite, "along with this.");

#ifdef CONFIG_KUNIT_DEBUGFS
	KUNIT_EXPECT_TRUE(test, test->log->append_newlines);

	full_log = string_stream_get_string(test, test->log, GFP_KERNEL);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test,
				     strstr(full_log, "put this in log."));
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test,
				     strstr(full_log, "this too."));

	full_log = string_stream_get_string(test, suite.log, GFP_KERNEL);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test,
				     strstr(full_log, "add to suite log."));
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test,
				     strstr(full_log, "along with this."));
#else
	KUNIT_EXPECT_NULL(test, test->log);
#endif
}

static void kunit_log_newline_test(struct kunit *test)
{
	char *full_log;

	kunit_info(test, "Add newline\n");
	if (test->log) {
		full_log = string_stream_get_string(test, test->log, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL_MSG(test, strstr(full_log, "Add newline\n"),
			"Missing log line, full log:\n%s", full_log);
		KUNIT_EXPECT_NULL(test, strstr(full_log, "Add newline\n\n"));
	} else {
		kunit_skip(test, "only useful when debugfs is enabled");
	}
}

static struct kunit_case kunit_log_test_cases[] = {
	KUNIT_CASE(kunit_log_test),
	KUNIT_CASE(kunit_log_newline_test),
	{}
};

static struct kunit_suite kunit_log_test_suite = {
	.name = "kunit-log-test",
	.test_cases = kunit_log_test_cases,
};

kunit_test_suites(&kunit_log_test_suite);
