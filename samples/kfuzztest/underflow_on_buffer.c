// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains a KFuzzTest example target that ensures that a buffer
 * underflow on a region triggers a KASAN OOB access report.
 *
 * Copyright 2025 Google LLC
 */

/**
 * test_underflow_on_buffer - a sample fuzz target
 *
 * This sample fuzz target serves to illustrate the usage of the
 * FUZZ_TEST_SIMPLE macro, as well as provide a sort of self-test that KFuzzTest
 * functions correctly for trivial fuzz targets. In KASAN builds, fuzzing this
 * harness should trigger a report for every input (provided that its length is
 * greater than 0 and less than KFUZZTEST_MAX_INPUT_SIZE).
 *
 * This harness can be invoked (naively) like so:
 * head -c 128 /dev/urandom > \
 *	/sys/kernel/debug/kfuzztest/test_underflow_on_buffer/input_simple
 */
#include <linux/kfuzztest.h>

static void underflow_on_buffer(char *buf, size_t buflen)
{
	size_t i;

	/*
	 * Print the address range of `buf` to allow correlation with the
	 * subsequent KASAN report.
	 */
	pr_info("buf = [%px, %px)", buf, buf + buflen);

	/* First ensure that all bytes in `buf` are accessible. */
	for (i = 0; i < buflen; i++)
		READ_ONCE(buf[i]);
	/*
	 * Provoke a buffer underflow on the first byte preceding `buf`,
	 * triggering a KASAN report.
	 */
	READ_ONCE(*((char *)buf - 1));
}

/**
 * Define the fuzz target. This wrapper ensures that the `underflow_on_buffer`
 * function is invoked with the data provided from userspace.
 */
FUZZ_TEST_SIMPLE(test_underflow_on_buffer)
{
	underflow_on_buffer(data, datalen);
	return 0;
}
