// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains a KFuzzTest example target that ensures that a buffer
 * underflow on a region triggers a KASAN OOB access report.
 *
 * Copyright 2025 Google LLC
 */

/**
 * DOC: test_underflow_on_buffer
 *
 * This test ensures that the region between the metadata struct and the
 * dynamically allocated buffer is poisoned. It provokes a one-byte underflow
 * on the buffer, which should be caught by KASAN.
 *
 * It can be invoked with kfuzztest-bridge using the following command:
 *
 * ./kfuzztest-bridge \
 *   "some_buffer { ptr[buf] len[buf, u64]}; buf { arr[u8, 128] };" \
 *   "test_underflow_on_buffer" /dev/urandom
 *
 * The first argument describes the C struct `some_buffer` and specifies that
 * `buf` is a pointer to an array of 128 bytes. The second argument is the test
 * name, and the third is a seed file.
 */
#include <linux/kfuzztest.h>

static void underflow_on_buffer(char *buf, size_t buflen)
{
	size_t i;

	pr_info("buf = [%px, %px)", buf, buf + buflen);

	/* First ensure that all bytes in arg->b are accessible. */
	for (i = 0; i < buflen; i++)
		READ_ONCE(buf[i]);
	/*
	 * Provoke a buffer overflow on the first byte preceding b, triggering
	 * a KASAN report.
	 */
	READ_ONCE(*((char *)buf - 1));
}

/**
 * Tests that the region between struct some_buffer and the expanded *buf field
 * is correctly poisoned by accessing the first byte before *buf.
 */
FUZZ_TEST_SIMPLE(test_underflow_on_buffer)
{
	underflow_on_buffer(data, datalen);
}
