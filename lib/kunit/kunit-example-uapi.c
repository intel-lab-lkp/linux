// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit Userspace example test.
 *
 * Copyright (C) 2025, Linutronix GmbH.
 * Author: Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 *
 * This is *userspace* code.
 */

#include "../../tools/testing/selftests/kselftest.h"

int main(void)
{
	ksft_print_header();
	ksft_set_plan(4);
	ksft_test_result_pass("userspace test 1\n");
	ksft_test_result_pass("userspace test 2\n");
	ksft_test_result_skip("userspace test 3: some reason\n");
	ksft_test_result_pass("userspace test 4\n");
	ksft_finished();
}
