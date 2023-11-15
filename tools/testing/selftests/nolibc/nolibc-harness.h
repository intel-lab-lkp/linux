/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Test harness for NOLIBC
 *
 * Copyright (C) 2023 Thomas Weißschuh <linux@weissschuh.net>
 * Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void putcharn(char c, size_t n)
{
	char buf[64];

	memset(buf, c, n);
	buf[n] = '\0';
	fputs(buf, stdout);
}

struct __test_setup;

struct __test_execution {
	int finished, failed, skipped, llen;
};

struct __test_metadata {
	const char *name;
	const char *suite;
	unsigned int suite_count;
	void (*func)(struct __test_metadata *metadata);
	struct __test_metadata *next;
	struct __test_setup *setup;
	struct __test_execution exe;
};

struct __test_setup {
	struct __test_metadata *start, *end;
} __test_setup __attribute__((weak));

static void __add_metadata(struct __test_metadata *metadata)
{
	struct __test_metadata *m;

	if (!__test_setup.start)
		__test_setup.start = metadata;

	if (!__test_setup.end) {
		__test_setup.end = metadata;
	} else {
		__test_setup.end->next = metadata;
		__test_setup.end = metadata;
	}

	metadata->setup = &__test_setup;

	for (m = __test_setup.start; m; m = m->next) {
		if (strcmp(metadata->suite, m->suite) == 0)
			metadata->suite_count++;
	}
}

#define TEST(_suite, _name) \
	static void _testfunc_ ## _suite ## _name(struct __test_metadata *); \
	static struct __test_metadata _metadata_ ## _suite ## _name = { \
		.name = #_name, \
		.suite = #_suite, \
		.func = _testfunc_ ## _suite ## _name, \
	}; \
	__attribute__((constructor)) \
	static void _register_testfunc_ ## _suite ## _name(void) \
	{ __add_metadata(&_metadata_ ## _suite ## _name); } \
	static void _testfunc_ ## _suite ## _name( \
		struct __test_metadata *_metadata __attribute__((unused)))

#define SKIP(statement) __extension__ ({ \
	_metadata->exe.skipped = 1; \
	statement; \
})

#define FAIL(statement) __extension__ ({ \
	_metadata->exe.failed = 1; \
	statement; \
})

static void __run_test(struct __test_metadata *metadata)
{
	metadata->func(metadata);
	metadata->exe.finished = 1;
}

static __attribute__((unused))
unsigned int count_test_suites(void)
{
	struct __test_metadata *m;
	unsigned int r = 0;

	for (m = __test_setup.start; m && m->suite_count; m = m->next)
		r++;

	return r;
}

static __attribute__((unused))
int count_tests(void)
{
	struct __test_metadata *m;
	unsigned int r = 0;

	for (m = __test_setup.start; m; m = m->next)
		r++;

	return r;
}

static unsigned int count_suite_tests(const char *suite)
{
	struct __test_metadata *m;
	unsigned int c = 0;

	for (m = __test_setup.start; m && m->suite_count; m = m->next)
		if (strcmp(m->suite, suite) == 0)
			c = m->suite_count;

	return c;
}

static __attribute__((unused))
void dump_test_plan(void)
{
	struct __test_metadata *m;
	const char *suite = "";

	printf("PLAN:\n");
	for (m = __test_setup.start; m; m = m->next) {
		if (strcmp(suite, m->suite)) {
			suite = m->suite;
			printf("  Suite %s (%d):\n", suite, count_suite_tests(suite));
		}
		printf("    %10s:%s %d\n", m->suite, m->name, m->suite_count - 1);
	}
}

static unsigned int run_test_suite(const char *suite, int min, int max)
{
	struct __test_metadata *m;
	const char *status;
	unsigned int errors = 0;
	int printed;

	for (m = __test_setup.start; m; m = m->next) {
		int testnum = m->suite_count - 1;

		if (strcmp(suite, m->suite) == 0 && testnum >= min && testnum <= max) {
			printed = printf("%d %s", testnum, m->name);
			__run_test(m);
			printed += m->exe.llen;
			if (printed < 64)
				putcharn(' ', 64 - printed);

			if (m->exe.failed)
				status = " [FAIL]";
			else if (m->exe.skipped)
				status = "[SKIPPED]";
			else
				status = "  [OK]";

			printf("%s\n", status);
			if (m->exe.failed)
				errors++;
		}
	}
	return errors;
};

static __attribute__((unused))
void reset_tests(void)
{
	struct __test_metadata *m;

	for (m = __test_setup.start; m; m = m->next)
		memset(&m->exe, 0, sizeof(m->exe));
}

static __attribute__((unused))
unsigned int run_all_tests(void)
{
	struct __test_metadata *m;
	unsigned int suite_errors, errors = 0;

	for (m = __test_setup.start; m; m = m->next) {
		if (!m->exe.finished) {
			printf("Running test '%s'\n", m->suite);
			suite_errors = run_test_suite(m->suite, 0, INT_MAX);
			printf("Errors during this test: %d\n\n", suite_errors);
			errors += suite_errors;
		}
	}

	printf("Total number of errors: %d\n", errors);
	return errors;
}

#define ASSERT_EQ(expected, seen) \
	__ASSERT(expected, #expected, seen, #seen, ==)
#define ASSERT_NE(expected, seen) \
	__ASSERT(expected, #expected, seen, #seen, !=)
#define ASSERT_LT(expected, seen) \
	__ASSERT(expected, #expected, seen, #seen, <)
#define ASSERT_LE(expected, seen) \
	__ASSERT(expected, #expected, seen, #seen, <=)
#define ASSERT_GT(expected, seen) \
	__ASSERT(expected, #expected, seen, #seen, >)
#define ASSERT_GE(expected, seen) \
	__ASSERT(expected, #expected, seen, #seen, >=)
#define ASSERT_NULL(seen) \
	__ASSERT(NULL, "NULL", seen, #seen, ==)
#define ASSERT_TRUE(seen) \
	__ASSERT(0, "0", seen, #seen, !=)
#define ASSERT_FALSE(seen) \
	__ASSERT(0, "0", seen, #seen, ==)

#define is_signed_type(var)       (!!(((__typeof__(var))(-1)) < (__typeof__(var))1))
#define is_pointer_type(var)	(__builtin_classify_type(var) == 5)

#define __ASSERT(_expected, _expected_str, _seen, _seen_str, _t) __extension__ ({ \
	/* Avoid multiple evaluation of the cases */ \
	__typeof__(_expected) __exp = (_expected); \
	__typeof__(_seen) __seen = (_seen); \
	int __ok = __exp _t __seen; \
	if (!__ok) \
		_metadata->exe.failed = 1; \
	if (is_pointer_type(__exp)) { \
		void * __expected_print = (void *)(uintptr_t)__exp; \
		_metadata->exe.llen = printf(" = <%p> ", __expected_print); \
	} else if (is_signed_type(__exp)) { \
		long long __expected_print = (intptr_t)__exp; \
		_metadata->exe.llen = printf(" = %lld ", __expected_print); \
	} else { \
		unsigned long long __expected_print = (uintptr_t)__exp; \
		_metadata->exe.llen = printf(" = %llu ", __expected_print); \
	} \
	__ok; \
})

#define ASSERT_STREQ(expected, seen) \
	__ASSERT_STR(expected, seen, ==)
#define ASSERT_STRNE(expected, seen) \
	__ASSERT_STR(expected, seen, !=)

#define __ASSERT_STR(_expected, _seen, _t) __extension__ ({ \
	const char *__exp = (_expected); \
	const char *__seen = (_seen); \
	int __ok = __seen && __exp && strcmp(__exp, __seen) _t 0; \
	if (!__ok) \
		_metadata->exe.failed = 1; \
	_metadata->exe.llen = printf(" = <%s> ", __exp ? __exp : ""); \
	__ok; \
})

#define ASSERT_STRNZ(seen) __extension__ ({ \
	const char *__seen = (seen); \
	int __ok = !!__seen; \
	if (!__ok) \
		_metadata->exe.failed = 1; \
	_metadata->exe.llen = printf(" = <%s> ", __seen); \
	__ok; \
})
