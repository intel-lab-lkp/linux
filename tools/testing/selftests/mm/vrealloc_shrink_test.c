// SPDX-License-Identifier: GPL-2.0
/*
 * Test that vrealloc() frees physical pages when shrinking.
 *
 * vrealloc() shrink path previously zeroed unused memory and updated
 * vm->requested_size, but never freed the physical pages backing the
 * unused portion of the allocation. This test verifies the fix by
 * loading a kernel module that directly measures nr_pages before and
 * after vrealloc() shrink.
 *
 * Copyright (C) 2026 Jill Ravaliya
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../kselftest.h"

#define MODULE_NAME "vrealloc_shrink_mod"
#define DMESG_PASS  "vrealloc_shrink: PASS"
#define DMESG_FAIL  "vrealloc_shrink: FAIL"

static int run_cmd(const char *cmd)
{
	return system(cmd);
}

static int check_dmesg_for(const char *pattern)
{
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
		 "dmesg | grep -q '%s'", pattern);
	return system(cmd) == 0;
}

int main(void)
{
	ksft_print_header();
	ksft_set_plan(1);

	/* Insert the test module */
	if (run_cmd("insmod " MODULE_NAME ".ko") != 0) {
		ksft_test_result_skip(
			"could not load %s.ko - is it built?\n",
			MODULE_NAME);
		ksft_finished();
	}

	/* Check dmesg for pass/fail */
	if (check_dmesg_for(DMESG_PASS)) {
		ksft_test_result_pass(
			"vrealloc shrink frees physical pages\n");
	} else if (check_dmesg_for(DMESG_FAIL)) {
		ksft_test_result_fail(
			"vrealloc shrink did NOT free physical pages\n");
	} else {
		ksft_test_result_fail(
			"could not find test result in dmesg\n");
	}

	run_cmd("rmmod " MODULE_NAME);
	ksft_finished();
}
