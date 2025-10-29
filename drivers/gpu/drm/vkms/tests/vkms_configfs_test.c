// SPDX-License-Identifier: GPL-2.0+

#include "linux/printk.h"
#include <kunit/test.h>

#include "../vkms_configfs.h"

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

/**
 * struct vkms_configfs_parse_format_case - Store test case for format parsing
 * @data: Contains the string to parse
 * @data_len: data len
 * @expected_len: expected len of the matched format
 * @expected_offset: expected offset in the string for the parsed format
 */
struct vkms_configfs_parse_format_case {
	const char *data;
	int data_len;
	int expected_len;
	int expected_offset;
};

struct vkms_configfs_parse_format_case vkms_configfs_parse_format_test_cases[] = {
	{
		.data = "+RG24",
		.data_len = 6,
		.expected_len = 5,
		.expected_offset = 0,
	}, {
		.data = "-RG24",
		.data_len = 6,
		.expected_len = 5,
		.expected_offset = 0
	}, {
		.data = "  -RG24",
		.data_len = 8,
		.expected_len = 5,
		.expected_offset = 2
	}, {
		.data = "+*",
		.data_len = 3,
		.expected_len = 2,
		.expected_offset = 0
	}, {
		.data = "-RG24+RG24",
		.data_len = 11,
		.expected_len = 5,
		.expected_offset = 0
	}, {
		.data = "-R1+RG24",
		.data_len = 9,
		.expected_len = 3,
		.expected_offset = 0
	}, {
		.data = "\n-R1",
		.data_len = 5,
		.expected_len = 3,
		.expected_offset = 1
	}, {
		.data = "-R1111",
		.data_len = 3,
		.expected_len = 3,
		.expected_offset = 0
	}
};

static void vkms_configfs_test_parse_format(struct kunit *test)
{
	const struct vkms_configfs_parse_format_case *param = test->param_value;
	char *out;
	int len = vkms_configfs_parse_next_format(param->data, param->data + param->data_len, &out);

	KUNIT_EXPECT_EQ(test, len, param->expected_len);
	KUNIT_EXPECT_PTR_EQ(test, out, param->data + param->expected_offset);
}

static void vkms_configfs_test_parse_format_get_desc(struct vkms_configfs_parse_format_case *t,
						     char *desc)
{
	snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", t->data);
}

KUNIT_ARRAY_PARAM(vkms_configfs_test_parse_format, vkms_configfs_parse_format_test_cases,
		  vkms_configfs_test_parse_format_get_desc
);

static struct kunit_case vkms_configfs_test_cases[] = {
	KUNIT_CASE_PARAM(vkms_configfs_test_parse_format,
			 vkms_configfs_test_parse_format_gen_params),
	{}
};

static struct kunit_suite vkms_configfs_test_suite = {
	.name = "vkms-configfs",
	.test_cases = vkms_configfs_test_cases,
};

kunit_test_suite(vkms_configfs_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kunit test for vkms configfs utility");
