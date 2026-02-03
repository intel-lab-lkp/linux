// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for argv_split module.
 */

#include <kunit/test.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/module.h>

struct argv_split_test_case {
	const char *str;
	const char *input;
	const int argc;
	const char *const *argv;
};

KUNIT_DEFINE_ACTION_WRAPPER(argv_free_wrapper, argv_free, char **);

static struct argv_split_test_case argv_split_cases[] = {
	{
		.str = "basic words",
		.input = "foo bar",
		.argc = 2,
		.argv = (const char *[]){ "foo", "bar" },
	},
	{
		.str = "single word",
		.input = "foobar",
		.argc = 1,
		.argv = (const char *[]){ "foobar" },
	},
	{
		.str = "leading/trailing whitespace",
		.input = "   hello  world ",
		.argc = 2,
		.argv = (const char *[]){ "hello", "world" },
	},
	{
		.str = "mixing space",
		.input = " \t foo \n bar   baz",
		.argc = 3,
		.argv = (const char *[]){ "foo", "bar", "baz" },
	},
	{
		.str = "quotes are treated as literals",
		.input = "ls \"my file/\"",
		.argc = 3,
		.argv = (const char *[]){ "ls", "\"my", "file/\"" },
	},
	{
		.str = "empty",
		.input = "",
		.argc = 0,
		.argv = NULL,
	},
};

KUNIT_ARRAY_PARAM_DESC(argv_split, argv_split_cases, str);

static void test_argv_split(struct kunit *test)
{
	const struct argv_split_test_case *params = test->param_value;
	int argc;
	char **argv;
	int i;

	argv = argv_split(GFP_KERNEL, params->input, &argc);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, argv);
	kunit_add_action(test, argv_free_wrapper, argv);

	KUNIT_EXPECT_EQ(test, argc, params->argc);
	for (i = 0; i < argc; i++)
		KUNIT_EXPECT_STREQ(test, argv[i], params->argv[i]);

	KUNIT_EXPECT_NULL(test, argv[argc]);
}

static struct kunit_case argv_split_test_cases[] = {
	KUNIT_CASE_PARAM(test_argv_split, argv_split_gen_params),
	{},
};

static struct kunit_suite argv_split_test_suite = {
	.name = "argv_split",
	.test_cases = argv_split_test_cases,
};

kunit_test_suite(argv_split_test_suite);

MODULE_AUTHOR("Ryota Sakamoto <sakamo.ryota@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("argv_split testing module");
