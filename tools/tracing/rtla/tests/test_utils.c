// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for src/utils.c parsing functions
 */

#define _GNU_SOURCE
#include <check.h>
#include <unistd.h>

#include "../src/utils.h"

START_TEST(test_parse_cpu_set)
{
	cpu_set_t set;
	int nr_cpus = sysconf(_SC_NPROCESSORS_CONF);

	ck_assert_int_eq(parse_cpu_set("0", &set), 0);
	ck_assert(CPU_ISSET(0, &set));
	ck_assert(!CPU_ISSET(1, &set));

	if (nr_cpus > 2) {
		ck_assert_int_eq(parse_cpu_set("0,2", &set), 0);
		ck_assert(CPU_ISSET(0, &set));
		ck_assert(CPU_ISSET(2, &set));
	}

	if (nr_cpus > 3) {
		ck_assert_int_eq(parse_cpu_set("0-3", &set), 0);
		ck_assert(CPU_ISSET(0, &set));
		ck_assert(CPU_ISSET(1, &set));
		ck_assert(CPU_ISSET(2, &set));
		ck_assert(CPU_ISSET(3, &set));
	}

	if (nr_cpus > 5) {
		ck_assert_int_eq(parse_cpu_set("1-3,5", &set), 0);
		ck_assert(!CPU_ISSET(0, &set));
		ck_assert(CPU_ISSET(1, &set));
		ck_assert(CPU_ISSET(2, &set));
		ck_assert(CPU_ISSET(3, &set));
		ck_assert(!CPU_ISSET(4, &set));
		ck_assert(CPU_ISSET(5, &set));
	}

	ck_assert_int_ne(parse_cpu_set("-1", &set), 0);
	ck_assert_int_ne(parse_cpu_set("abc", &set), 0);
	ck_assert_int_ne(parse_cpu_set("9999", &set), 0);
}
END_TEST

Suite *utils_suite(void)
{
	Suite *s = suite_create("utils");
	TCase *tc = tcase_create("core");

	tcase_add_test(tc, test_parse_cpu_set);

	suite_add_tcase(s, tc);
	return s;
}

int main(void)
{
	int num_failed;
	SRunner *sr;

	sr = srunner_create(utils_suite());
	srunner_run_all(sr, CK_NORMAL);
	num_failed = srunner_ntests_failed(sr);

	srunner_free(sr);

	return (num_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
