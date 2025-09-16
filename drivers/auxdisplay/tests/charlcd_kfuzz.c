// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * charlcd KFuzzTest target
 *
 * Copyright 2025 Google LLC
 */
#include <linux/kfuzztest.h>

struct parse_xy_arg {
	const char *s;
};

FUZZ_TEST(test_parse_xy, struct parse_xy_arg)
{
	unsigned long x, y;

	KFUZZTEST_EXPECT_NOT_NULL(parse_xy_arg, s);
	KFUZZTEST_ANNOTATE_STRING(parse_xy_arg, s);
	parse_xy(arg->s, &x, &y);
}
