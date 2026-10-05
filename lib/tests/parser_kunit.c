// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for the number helpers in lib/parser.c
 */

#include <kunit/test.h>
#include <linux/limits.h>
#include <linux/parser.h>
#include <linux/string.h>

struct parser_number_case {
	const char *in;
	int ret;
	int val;
};

typedef int (*match_fn_t)(substring_t *s, int *result);

static void parser_check(struct kunit *test, match_fn_t fn, const char *name,
			 const struct parser_number_case *c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		char buf[64];
		substring_t s;
		int val = 0x5a5a5a5a;
		int ret;

		strscpy(buf, c[i].in, sizeof(buf));
		s.from = buf;
		s.to = buf + strlen(buf);

		ret = fn(&s, &val);
		KUNIT_EXPECT_EQ_MSG(test, ret, c[i].ret, "%s(\"%s\")",
				    name, c[i].in);
		if (!c[i].ret)
			KUNIT_EXPECT_EQ_MSG(test, val, c[i].val, "%s(\"%s\")",
					    name, c[i].in);
	}
}

static const struct parser_number_case match_int_cases[] = {
	{ "0",				0,		0 },
	{ "42",				0,		42 },
	{ "-42",			0,		-42 },
	{ "0x10",			0,		16 },
	{ "010",			0,		8 },
	{ "2147483647",			0,		INT_MAX },
	{ "-2147483648",		0,		INT_MIN },
	{ "2147483648",			-ERANGE,	0 },
	{ "-2147483649",		-ERANGE,	0 },
	{ "4294967295",			-ERANGE,	0 },
	{ "9223372036854775808",	-ERANGE,	0 },
	{ "18446744073709551615",	-ERANGE,	0 },
	{ "-18446744073709551615",	-ERANGE,	0 },
	{ "99999999999999999999",	-ERANGE,	0 },
	{ "0xffffffffffffffff",		-ERANGE,	0 },
	{ "abc",			-EINVAL,	0 },
};

static const struct parser_number_case match_octal_cases[] = {
	{ "755",			0,		0755 },
	{ "17777777777",		0,		INT_MAX },
	{ "20000000000",		-ERANGE,	0 },
	{ "1777777777777777777777",	-ERANGE,	0 },
	{ "8",				-EINVAL,	0 },
};

static const struct parser_number_case match_hex_cases[] = {
	{ "ff",				0,		0xff },
	{ "7fffffff",			0,		INT_MAX },
	{ "80000000",			-ERANGE,	0 },
	{ "ffffffffffffffff",		-ERANGE,	0 },
	{ "10000000000000000",		-ERANGE,	0 },
	{ "g",				-EINVAL,	0 },
};

static void parser_test_match_int(struct kunit *test)
{
	parser_check(test, match_int, "match_int", match_int_cases,
		     ARRAY_SIZE(match_int_cases));
}

static void parser_test_match_octal(struct kunit *test)
{
	parser_check(test, match_octal, "match_octal", match_octal_cases,
		     ARRAY_SIZE(match_octal_cases));
}

static void parser_test_match_hex(struct kunit *test)
{
	parser_check(test, match_hex, "match_hex", match_hex_cases,
		     ARRAY_SIZE(match_hex_cases));
}

enum { PARSER_TEST_OPT_SIZE, PARSER_TEST_OPT_ERR };

static const match_table_t parser_test_tokens = {
	{ PARSER_TEST_OPT_SIZE, "size=%d" },
	{ PARSER_TEST_OPT_ERR, NULL },
};

static void parser_test_match_token_int(struct kunit *test)
{
	char opt[] = "size=18446744073709551615";
	substring_t args[MAX_OPT_ARGS];
	int val = 0;

	KUNIT_ASSERT_EQ(test, match_token(opt, parser_test_tokens, args),
			PARSER_TEST_OPT_SIZE);
	KUNIT_EXPECT_EQ(test, match_int(&args[0], &val), -ERANGE);
}

static struct kunit_case parser_test_cases[] = {
	KUNIT_CASE(parser_test_match_int),
	KUNIT_CASE(parser_test_match_octal),
	KUNIT_CASE(parser_test_match_hex),
	KUNIT_CASE(parser_test_match_token_int),
	{}
};

static struct kunit_suite parser_test_suite = {
	.name = "parser",
	.test_cases = parser_test_cases,
};

kunit_test_suite(parser_test_suite);

MODULE_DESCRIPTION("KUnit tests for lib/parser.c number helpers");
MODULE_LICENSE("GPL");
