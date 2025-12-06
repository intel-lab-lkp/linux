// SPDX-License-Identifier: LGPL-2.1
/*
 *
 *   KUnit tests of SMB2 maperror
 *
 *   Copyright (C) 2025 KylinSoft Co., Ltd. All rights reserved.
 *   Author(s): ChenXiaoSong <chenxiaosong@kylinos.cn>
 *
 */

#include <kunit/test.h>

static void maperror_test_check_sort(struct kunit *test)
{
	bool is_sorted = true;
	unsigned int i;

	for (i = 1; i < err_map_num; i++) {
		if (smb2_error_map_table[i].smb2_status >=
		    smb2_error_map_table[i - 1].smb2_status)
			continue;

		pr_err("smb2_error_map_table array order is incorrect\n");
		is_sorted = false;
		break;
	}

	KUNIT_EXPECT_EQ(test, true, is_sorted);
}

static void
test_cmp_map(struct kunit *test, struct status_to_posix_error *expect)
{
	struct status_to_posix_error *result;

	result = smb2_get_err_map(expect->smb2_status);
	KUNIT_EXPECT_PTR_NE(test, NULL, result);
	KUNIT_EXPECT_EQ(test, expect->smb2_status, result->smb2_status);
	KUNIT_EXPECT_EQ(test, expect->posix_error, result->posix_error);
	KUNIT_EXPECT_STREQ(test, expect->status_string, result->status_string);
}

static void maperror_test_check_search(struct kunit *test)
{
	unsigned int i;
	struct status_to_posix_error expect;

	for (i = 0; i < err_map_num; i++) {
		expect = smb2_error_map_table[i];
		test_cmp_map(test, &expect);
	}
}

/*
 * Before running these test cases, the smb2_init_maperror()
 * function is called first.
 */
static struct kunit_case maperror_test_cases[] = {
	KUNIT_CASE(maperror_test_check_sort),
	KUNIT_CASE(maperror_test_check_search),
	{}
};

static struct kunit_suite maperror_suite = {
	.name = "smb2_maperror",
	.test_cases = maperror_test_cases,
};

kunit_test_suite(maperror_suite);

MODULE_LICENSE("GPL");
