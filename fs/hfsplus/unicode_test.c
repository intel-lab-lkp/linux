// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for HFS+ Unicode string operations
 *
 * Copyright (C) 2025 Viacheslav Dubeyko <slava@dubeyko.com>
 */

#include <kunit/test.h>
#include <linux/nls.h>
#include <linux/dcache.h>
#include <linux/stringhash.h>
#include "hfsplus_fs.h"

/* Helper function to create hfsplus_unistr */
static void create_unistr(struct hfsplus_unistr *ustr, const char *ascii_str)
{
	int len = strlen(ascii_str);
	int i;

	memset(ustr->unicode, 0, sizeof(ustr->unicode));

	ustr->length = cpu_to_be16(len);
	for (i = 0; i < len && i < HFSPLUS_MAX_STRLEN; i++)
		ustr->unicode[i] = cpu_to_be16((u16)ascii_str[i]);
}

static void corrupt_unistr(struct hfsplus_unistr *ustr)
{
	ustr->length = cpu_to_be16(U16_MAX);
}

/* Test hfsplus_strcasecmp function */
static void hfsplus_strcasecmp_test(struct kunit *test)
{
	struct hfsplus_unistr str1, str2;
	char long_str[HFSPLUS_MAX_STRLEN + 1];

	/* Test identical strings */
	create_unistr(&str1, "hello");
	create_unistr(&str2, "hello");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	/* Test case insensitive comparison */
	create_unistr(&str1, "Hello");
	create_unistr(&str2, "hello");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	create_unistr(&str1, "HELLO");
	create_unistr(&str2, "hello");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	/* Test different strings */
	create_unistr(&str1, "apple");
	create_unistr(&str2, "banana");
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	create_unistr(&str1, "zebra");
	create_unistr(&str2, "apple");
	KUNIT_EXPECT_GT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	/* Test different lengths */
	create_unistr(&str1, "test");
	create_unistr(&str2, "testing");
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	create_unistr(&str1, "testing");
	create_unistr(&str2, "test");
	KUNIT_EXPECT_GT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	/* Test empty strings */
	create_unistr(&str1, "");
	create_unistr(&str2, "");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	create_unistr(&str1, "");
	create_unistr(&str2, "test");
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	/* Test single characters */
	create_unistr(&str1, "A");
	create_unistr(&str2, "a");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	create_unistr(&str1, "A");
	create_unistr(&str2, "B");
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	/* Test maximum length strings */
	memset(long_str, 'a', HFSPLUS_MAX_STRLEN);
	long_str[HFSPLUS_MAX_STRLEN] = '\0';
	create_unistr(&str1, long_str);
	create_unistr(&str2, long_str);
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	/* Change one character in the middle */
	long_str[HFSPLUS_MAX_STRLEN / 2] = 'b';
	create_unistr(&str2, long_str);
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	/* Test corrupted strings */
	create_unistr(&str1, "");
	corrupt_unistr(&str1);
	create_unistr(&str2, "");
	KUNIT_EXPECT_NE(test, 0, hfsplus_strcasecmp(&str1, &str2));

	create_unistr(&str1, "");
	create_unistr(&str2, "");
	corrupt_unistr(&str2);
	KUNIT_EXPECT_NE(test, 0, hfsplus_strcasecmp(&str1, &str2));

	create_unistr(&str1, "test");
	corrupt_unistr(&str1);
	create_unistr(&str2, "testing");
	KUNIT_EXPECT_GT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	create_unistr(&str1, "test");
	create_unistr(&str2, "testing");
	corrupt_unistr(&str2);
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	create_unistr(&str1, "testing");
	corrupt_unistr(&str1);
	create_unistr(&str2, "test");
	KUNIT_EXPECT_GT(test, hfsplus_strcasecmp(&str1, &str2), 0);

	create_unistr(&str1, "testing");
	create_unistr(&str2, "test");
	corrupt_unistr(&str2);
	KUNIT_EXPECT_LT(test, hfsplus_strcasecmp(&str1, &str2), 0);
}

/* Test hfsplus_strcmp function (case-sensitive) */
static void hfsplus_strcmp_test(struct kunit *test)
{
	struct hfsplus_unistr str1, str2;
	char long_str[HFSPLUS_MAX_STRLEN + 1];

	/* Test identical strings */
	create_unistr(&str1, "hello");
	create_unistr(&str2, "hello");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));

	/* Test case sensitive comparison - should NOT be equal */
	create_unistr(&str1, "Hello");
	create_unistr(&str2, "hello");
	KUNIT_EXPECT_NE(test, 0, hfsplus_strcmp(&str1, &str2));
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0); /* 'H' < 'h' in Unicode */

	/* Test lexicographic ordering */
	create_unistr(&str1, "apple");
	create_unistr(&str2, "banana");
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0);

	create_unistr(&str1, "zebra");
	create_unistr(&str2, "apple");
	KUNIT_EXPECT_GT(test, hfsplus_strcmp(&str1, &str2), 0);

	/* Test different lengths with common prefix */
	create_unistr(&str1, "test");
	create_unistr(&str2, "testing");
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0);

	create_unistr(&str1, "testing");
	create_unistr(&str2, "test");
	KUNIT_EXPECT_GT(test, hfsplus_strcmp(&str1, &str2), 0);

	/* Test empty strings */
	create_unistr(&str1, "");
	create_unistr(&str2, "");
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));

	/* Test maximum length strings */
	memset(long_str, 'a', HFSPLUS_MAX_STRLEN);
	long_str[HFSPLUS_MAX_STRLEN] = '\0';
	create_unistr(&str1, long_str);
	create_unistr(&str2, long_str);
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));

	/* Change one character in the middle */
	long_str[HFSPLUS_MAX_STRLEN / 2] = 'b';
	create_unistr(&str2, long_str);
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0);

	/* Test corrupted strings */
	create_unistr(&str1, "");
	corrupt_unistr(&str1);
	create_unistr(&str2, "");
	KUNIT_EXPECT_NE(test, 0, hfsplus_strcmp(&str1, &str2));

	create_unistr(&str1, "");
	create_unistr(&str2, "");
	corrupt_unistr(&str2);
	KUNIT_EXPECT_NE(test, 0, hfsplus_strcmp(&str1, &str2));

	create_unistr(&str1, "test");
	corrupt_unistr(&str1);
	create_unistr(&str2, "testing");
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0);

	create_unistr(&str1, "test");
	create_unistr(&str2, "testing");
	corrupt_unistr(&str2);
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0);

	create_unistr(&str1, "testing");
	corrupt_unistr(&str1);
	create_unistr(&str2, "test");
	KUNIT_EXPECT_GT(test, hfsplus_strcmp(&str1, &str2), 0);

	create_unistr(&str1, "testing");
	create_unistr(&str2, "test");
	corrupt_unistr(&str2);
	KUNIT_EXPECT_GT(test, hfsplus_strcmp(&str1, &str2), 0);
}

/* Test Unicode edge cases */
static void hfsplus_unicode_edge_cases_test(struct kunit *test)
{
	struct hfsplus_unistr str1, str2;

	/* Test with special characters */
	str1.length = cpu_to_be16(3);
	str1.unicode[0] = cpu_to_be16(0x00E9); /* é */
	str1.unicode[1] = cpu_to_be16(0x00F1); /* ñ */
	str1.unicode[2] = cpu_to_be16(0x00FC); /* ü */

	str2.length = cpu_to_be16(3);
	str2.unicode[0] = cpu_to_be16(0x00E9); /* é */
	str2.unicode[1] = cpu_to_be16(0x00F1); /* ñ */
	str2.unicode[2] = cpu_to_be16(0x00FC); /* ü */

	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	/* Test with different special characters */
	str2.unicode[1] = cpu_to_be16(0x00F2); /* ò */
	KUNIT_EXPECT_NE(test, 0, hfsplus_strcmp(&str1, &str2));

	/* Test null characters within string (should be handled correctly) */
	str1.length = cpu_to_be16(3);
	str1.unicode[0] = cpu_to_be16('a');
	str1.unicode[1] = cpu_to_be16(0x0000); /* null */
	str1.unicode[2] = cpu_to_be16('b');

	str2.length = cpu_to_be16(3);
	str2.unicode[0] = cpu_to_be16('a');
	str2.unicode[1] = cpu_to_be16(0x0000); /* null */
	str2.unicode[2] = cpu_to_be16('b');

	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));
}

/* Test boundary conditions */
static void hfsplus_unicode_boundary_test(struct kunit *test)
{
	struct hfsplus_unistr str1, str2;
	int i;

	/* Test maximum length boundary */
	str1.length = cpu_to_be16(HFSPLUS_MAX_STRLEN);
	str2.length = cpu_to_be16(HFSPLUS_MAX_STRLEN);

	for (i = 0; i < HFSPLUS_MAX_STRLEN; i++) {
		str1.unicode[i] = cpu_to_be16('A');
		str2.unicode[i] = cpu_to_be16('A');
	}

	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));

	/* Change last character */
	str2.unicode[HFSPLUS_MAX_STRLEN - 1] = cpu_to_be16('B');
	KUNIT_EXPECT_LT(test, hfsplus_strcmp(&str1, &str2), 0);

	/* Test zero length strings */
	str1.length = cpu_to_be16(0);
	str2.length = cpu_to_be16(0);
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcmp(&str1, &str2));
	KUNIT_EXPECT_EQ(test, 0, hfsplus_strcasecmp(&str1, &str2));

	/* Test one character vs empty */
	str1.length = cpu_to_be16(1);
	str1.unicode[0] = cpu_to_be16('A');
	str2.length = cpu_to_be16(0);
	KUNIT_EXPECT_GT(test, hfsplus_strcmp(&str1, &str2), 0);
	KUNIT_EXPECT_GT(test, hfsplus_strcasecmp(&str1, &str2), 0);
}

/* Mock superblock and NLS table for testing hfsplus_uni2asc */
static struct nls_table test_nls;
static struct hfsplus_sb_info test_sb_info;
static struct super_block test_sb;

static void setup_mock_sb(void)
{
	memset(&test_nls, 0, sizeof(test_nls));
	memset(&test_sb_info, 0, sizeof(test_sb_info));
	memset(&test_sb, 0, sizeof(test_sb));

	test_nls.charset = "utf8";
	test_nls.uni2char = NULL; /* Will use default behavior */
	test_sb_info.nls = &test_nls;
	test_sb.s_fs_info = &test_sb_info;

	/* Set default flags - no decomposition, no case folding */
	clear_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);
	clear_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);
}

/* Simple uni2char implementation for testing */
static int test_uni2char(wchar_t uni, unsigned char *out, int boundlen)
{
	if (boundlen <= 0)
		return -ENAMETOOLONG;

	if (uni < 0x80) {
		*out = (unsigned char)uni;
		return 1;
	}

	/* For non-ASCII, just use '?' as fallback */
	*out = '?';
	return 1;
}

/* Test hfsplus_uni2asc basic functionality */
static void hfsplus_uni2asc_basic_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	char astr[256];
	int len, result;

	setup_mock_sb();
	test_nls.uni2char = test_uni2char;

	/* Test simple ASCII string conversion */
	create_unistr(&ustr, "hello");
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 5, len);
	KUNIT_EXPECT_STREQ(test, "hello", astr);

	/* Test empty string */
	create_unistr(&ustr, "");
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 0, len);

	/* Test single character */
	create_unistr(&ustr, "A");
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 1, len);
	KUNIT_EXPECT_EQ(test, 'A', astr[0]);
}

/* Test special character handling */
static void hfsplus_uni2asc_special_chars_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	char astr[256];
	int len, result;

	setup_mock_sb();
	test_nls.uni2char = test_uni2char;

	/* Test null character conversion (should become 0x2400) */
	ustr.length = cpu_to_be16(1);
	ustr.unicode[0] = cpu_to_be16(0x0000);
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 1, len);
	/* Our test implementation returns '?' for non-ASCII */
	KUNIT_EXPECT_EQ(test, '?', astr[0]);

	/* Test forward slash conversion (should become colon) */
	ustr.length = cpu_to_be16(1);
	ustr.unicode[0] = cpu_to_be16('/');
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 1, len);
	KUNIT_EXPECT_EQ(test, ':', astr[0]);

	/* Test string with mixed special characters */
	ustr.length = cpu_to_be16(3);
	ustr.unicode[0] = cpu_to_be16('a');
	ustr.unicode[1] = cpu_to_be16('/');
	ustr.unicode[2] = cpu_to_be16('b');
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 3, len);
	KUNIT_EXPECT_EQ(test, 'a', astr[0]);
	KUNIT_EXPECT_EQ(test, ':', astr[1]);
	KUNIT_EXPECT_EQ(test, 'b', astr[2]);
}

/* Test buffer length handling */
static void hfsplus_uni2asc_buffer_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	char astr[10];
	int len, result;

	setup_mock_sb();
	test_nls.uni2char = test_uni2char;

	/* Test insufficient buffer space */
	create_unistr(&ustr, "toolongstring");
	len = 5; /* Buffer too small */
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, -ENAMETOOLONG, result);
	KUNIT_EXPECT_EQ(test, 5, len); /* Should be set to consumed length */

	/* Test exact buffer size */
	create_unistr(&ustr, "exact");
	len = 5;
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 5, len);

	/* Test zero length buffer */
	create_unistr(&ustr, "test");
	len = 0;
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, -ENAMETOOLONG, result);
	KUNIT_EXPECT_EQ(test, 0, len);
}

/* Test corrupted unicode string handling */
static void hfsplus_uni2asc_corrupted_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	char astr[256];
	int len, result;

	setup_mock_sb();
	test_nls.uni2char = test_uni2char;

	/* Test corrupted length (too large) */
	create_unistr(&ustr, "test");
	corrupt_unistr(&ustr); /* Sets length to U16_MAX */
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	/* Should still work but with corrected length */
	KUNIT_EXPECT_EQ(test, 0, result);
	/*
	 * Length should be corrected to HFSPLUS_MAX_STRLEN
	 * and processed accordingly
	 */
	KUNIT_EXPECT_GT(test, len, 0);
}

/* Test edge cases and boundary conditions */
static void hfsplus_uni2asc_edge_cases_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	char astr[HFSPLUS_MAX_STRLEN * 2];
	int len, result;
	int i;

	setup_mock_sb();
	test_nls.uni2char = test_uni2char;

	/* Test maximum length string */
	ustr.length = cpu_to_be16(HFSPLUS_MAX_STRLEN);
	for (i = 0; i < HFSPLUS_MAX_STRLEN; i++)
		ustr.unicode[i] = cpu_to_be16('a');

	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, HFSPLUS_MAX_STRLEN, len);

	/* Verify all characters are 'a' */
	for (i = 0; i < HFSPLUS_MAX_STRLEN; i++)
		KUNIT_EXPECT_EQ(test, 'a', astr[i]);

	/* Test string with high Unicode values (non-ASCII) */
	ustr.length = cpu_to_be16(3);
	ustr.unicode[0] = cpu_to_be16(0x00E9); /* é */
	ustr.unicode[1] = cpu_to_be16(0x00F1); /* ñ */
	ustr.unicode[2] = cpu_to_be16(0x00FC); /* ü */
	len = sizeof(astr);
	result = hfsplus_uni2asc(&test_sb, &ustr, astr, &len);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 3, len);
	/* Our test implementation converts non-ASCII to '?' */
	KUNIT_EXPECT_EQ(test, '?', astr[0]);
	KUNIT_EXPECT_EQ(test, '?', astr[1]);
	KUNIT_EXPECT_EQ(test, '?', astr[2]);
}

/* Simple char2uni implementation for testing */
static int test_char2uni(const unsigned char *rawstring,
			 int boundlen, wchar_t *uni)
{
	if (boundlen <= 0)
		return -EINVAL;

	*uni = (wchar_t)*rawstring;
	return 1;
}

/* Helper function to check unicode string contents */
static void check_unistr_content(struct kunit *test,
				 struct hfsplus_unistr *ustr,
				 const char *expected_ascii)
{
	int expected_len = strlen(expected_ascii);
	int actual_len = be16_to_cpu(ustr->length);
	int i;

	KUNIT_EXPECT_EQ(test, expected_len, actual_len);

	for (i = 0; i < expected_len && i < actual_len; i++) {
		u16 expected_char = (u16)expected_ascii[i];
		u16 actual_char = be16_to_cpu(ustr->unicode[i]);

		KUNIT_EXPECT_EQ(test, expected_char, actual_char);
	}
}

/* Test hfsplus_asc2uni basic functionality */
static void hfsplus_asc2uni_basic_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	int result;

	setup_mock_sb();
	test_nls.char2uni = test_char2uni;

	/* Test simple ASCII string conversion */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "hello", 5);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr, "hello");

	/* Test empty string */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "", 0);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 0, be16_to_cpu(ustr.length));

	/* Test single character */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "A", 1);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr, "A");

	/* Test null-terminated string with explicit length */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "test\0extra", 4);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr, "test");
}

/* Test special character handling in asc2uni */
static void hfsplus_asc2uni_special_chars_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	int result;

	setup_mock_sb();
	test_nls.char2uni = test_char2uni;

	/* Test colon conversion (should become forward slash) */
	result = hfsplus_asc2uni(&test_sb, &ustr, HFSPLUS_MAX_STRLEN, ":", 1);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 1, be16_to_cpu(ustr.length));
	KUNIT_EXPECT_EQ(test, '/', be16_to_cpu(ustr.unicode[0]));

	/* Test string with mixed special characters */
	result = hfsplus_asc2uni(&test_sb, &ustr, HFSPLUS_MAX_STRLEN, "a:b", 3);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 3, be16_to_cpu(ustr.length));
	KUNIT_EXPECT_EQ(test, 'a', be16_to_cpu(ustr.unicode[0]));
	KUNIT_EXPECT_EQ(test, '/', be16_to_cpu(ustr.unicode[1]));
	KUNIT_EXPECT_EQ(test, 'b', be16_to_cpu(ustr.unicode[2]));

	/* Test multiple special characters */
	result = hfsplus_asc2uni(&test_sb, &ustr, HFSPLUS_MAX_STRLEN, ":::", 3);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 3, be16_to_cpu(ustr.length));
	KUNIT_EXPECT_EQ(test, '/', be16_to_cpu(ustr.unicode[0]));
	KUNIT_EXPECT_EQ(test, '/', be16_to_cpu(ustr.unicode[1]));
	KUNIT_EXPECT_EQ(test, '/', be16_to_cpu(ustr.unicode[2]));
}

/* Test buffer length limits */
static void hfsplus_asc2uni_buffer_limits_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	int result;
	char long_string[HFSPLUS_MAX_STRLEN + 10];

	setup_mock_sb();
	test_nls.char2uni = test_char2uni;

	/* Test exact maximum length */
	memset(long_string, 'a', HFSPLUS_MAX_STRLEN);
	result = hfsplus_asc2uni(&test_sb, &ustr, HFSPLUS_MAX_STRLEN,
				 long_string, HFSPLUS_MAX_STRLEN);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, HFSPLUS_MAX_STRLEN, be16_to_cpu(ustr.length));

	/* Test exceeding maximum length */
	memset(long_string, 'a', HFSPLUS_MAX_STRLEN + 5);
	result = hfsplus_asc2uni(&test_sb, &ustr, HFSPLUS_MAX_STRLEN,
				 long_string, HFSPLUS_MAX_STRLEN + 5);

	KUNIT_EXPECT_EQ(test, -ENAMETOOLONG, result);
	KUNIT_EXPECT_EQ(test, HFSPLUS_MAX_STRLEN, be16_to_cpu(ustr.length));

	/* Test with smaller max_unistr_len */
	result = hfsplus_asc2uni(&test_sb, &ustr, 5, "toolongstring", 13);

	KUNIT_EXPECT_EQ(test, -ENAMETOOLONG, result);
	KUNIT_EXPECT_EQ(test, 5, be16_to_cpu(ustr.length));

	/* Test zero max length */
	result = hfsplus_asc2uni(&test_sb, &ustr, 0, "test", 4);

	KUNIT_EXPECT_EQ(test, -ENAMETOOLONG, result);
	KUNIT_EXPECT_EQ(test, 0, be16_to_cpu(ustr.length));
}

/* Test error handling and edge cases */
static void hfsplus_asc2uni_edge_cases_test(struct kunit *test)
{
	struct hfsplus_unistr ustr;
	char test_str[] = {'a', '\0', 'b'};
	int result;

	setup_mock_sb();
	test_nls.char2uni = test_char2uni;

	/* Test zero length input */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "test", 0);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 0, be16_to_cpu(ustr.length));

	/* Test input with length mismatch */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "hello", 3);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr, "hel");

	/* Test with various printable ASCII characters */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, "ABC123!@#", 9);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr, "ABC123!@#");

	/* Test null character in the middle */
	result = hfsplus_asc2uni(&test_sb,
				 &ustr, HFSPLUS_MAX_STRLEN, test_str, 3);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, 3, be16_to_cpu(ustr.length));
	KUNIT_EXPECT_EQ(test, 'a', be16_to_cpu(ustr.unicode[0]));
	KUNIT_EXPECT_EQ(test, 0, be16_to_cpu(ustr.unicode[1]));
	KUNIT_EXPECT_EQ(test, 'b', be16_to_cpu(ustr.unicode[2]));
}

/* Test decomposition flag behavior */
static void hfsplus_asc2uni_decompose_test(struct kunit *test)
{
	struct hfsplus_unistr ustr1, ustr2;
	int result;

	setup_mock_sb();
	test_nls.char2uni = test_char2uni;

	/* Test with decomposition disabled (default) */
	clear_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);
	result = hfsplus_asc2uni(&test_sb,
				 &ustr1, HFSPLUS_MAX_STRLEN, "test", 4);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr1, "test");

	/* Test with decomposition enabled */
	set_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);
	result = hfsplus_asc2uni(&test_sb,
				 &ustr2, HFSPLUS_MAX_STRLEN, "test", 4);

	KUNIT_EXPECT_EQ(test, 0, result);
	check_unistr_content(test, &ustr2, "test");

	/* For simple ASCII, both should produce the same result */
	KUNIT_EXPECT_EQ(test,
			be16_to_cpu(ustr1.length), be16_to_cpu(ustr2.length));
}

/* Mock dentry for testing hfsplus_hash_dentry */
static struct dentry test_dentry;

static void setup_mock_dentry(void)
{
	memset(&test_dentry, 0, sizeof(test_dentry));
	test_dentry.d_sb = &test_sb;
}

/* Helper function to create qstr */
static void create_qstr(struct qstr *str, const char *name)
{
	str->name = name;
	str->len = strlen(name);
	str->hash = 0; /* Will be set by hash function */
}

/* Test hfsplus_hash_dentry basic functionality */
static void hfsplus_hash_dentry_basic_test(struct kunit *test)
{
	struct qstr str1, str2;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test basic string hashing */
	create_qstr(&str1, "hello");
	result = hfsplus_hash_dentry(&test_dentry, &str1);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_NE(test, 0, str1.hash);

	/* Test that identical strings produce identical hashes */
	create_qstr(&str2, "hello");
	result = hfsplus_hash_dentry(&test_dentry, &str2);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, str1.hash, str2.hash);

	/* Test empty string */
	create_qstr(&str1, "");
	result = hfsplus_hash_dentry(&test_dentry, &str1);

	/* Empty string should still produce a hash */
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test single character */
	create_qstr(&str1, "A");
	result = hfsplus_hash_dentry(&test_dentry, &str1);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_NE(test, 0, str1.hash);
}

/* Test case folding behavior in hash */
static void hfsplus_hash_dentry_casefold_test(struct kunit *test)
{
	struct qstr str1, str2;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test with case folding disabled (default) */
	clear_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);

	create_qstr(&str1, "Hello");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&str2, "hello");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	/*
	 * Without case folding, different cases
	 * should produce different hashes
	 */
	KUNIT_EXPECT_NE(test, str1.hash, str2.hash);

	/* Test with case folding enabled */
	set_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);

	create_qstr(&str1, "Hello");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&str2, "hello");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* With case folding, different cases should produce same hash */
	KUNIT_EXPECT_EQ(test, str1.hash, str2.hash);

	/* Test mixed case */
	create_qstr(&str1, "HeLLo");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_EQ(test, str1.hash, str2.hash);
}

/* Test special character handling in hash */
static void hfsplus_hash_dentry_special_chars_test(struct kunit *test)
{
	struct qstr str1, str2;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test colon conversion (: becomes /) */
	create_qstr(&str1, "file:name");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&str2, "file/name");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* After conversion, these should produce the same hash */
	KUNIT_EXPECT_EQ(test, str1.hash, str2.hash);

	/* Test multiple special characters */
	create_qstr(&str1, ":::");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&str2, "///");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	KUNIT_EXPECT_EQ(test, str1.hash, str2.hash);
}

/* Test decomposition flag behavior in hash */
static void hfsplus_hash_dentry_decompose_test(struct kunit *test)
{
	struct qstr str1, str2;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test with decomposition disabled (default) */
	clear_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);

	create_qstr(&str1, "test");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test with decomposition enabled */
	set_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);

	create_qstr(&str2, "test");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	/*
	 * For simple ASCII, decomposition shouldn't change
	 * the hash much but the function should still work correctly
	 */
	KUNIT_EXPECT_NE(test, 0, str2.hash);
}

/* Test hash consistency and distribution */
static void hfsplus_hash_dentry_consistency_test(struct kunit *test)
{
	struct qstr str1, str2, str3;
	unsigned long hash1;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test that same string always produces same hash */
	create_qstr(&str1, "consistent");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);
	hash1 = str1.hash;

	create_qstr(&str2, "consistent");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	KUNIT_EXPECT_EQ(test, hash1, str2.hash);

	/* Test that different strings produce different hashes */
	create_qstr(&str3, "different");
	result = hfsplus_hash_dentry(&test_dentry, &str3);
	KUNIT_EXPECT_EQ(test, 0, result);

	KUNIT_EXPECT_NE(test, str1.hash, str3.hash);

	/* Test similar strings should have different hashes */
	create_qstr(&str1, "file1");
	result = hfsplus_hash_dentry(&test_dentry, &str1);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&str2, "file2");
	result = hfsplus_hash_dentry(&test_dentry, &str2);
	KUNIT_EXPECT_EQ(test, 0, result);

	KUNIT_EXPECT_NE(test, str1.hash, str2.hash);
}

/* Test edge cases and boundary conditions */
static void hfsplus_hash_dentry_edge_cases_test(struct kunit *test)
{
	struct qstr str;
	int result;
	char long_name[256];

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test very long filename */
	memset(long_name, 'a', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';

	create_qstr(&str, long_name);
	result = hfsplus_hash_dentry(&test_dentry, &str);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_NE(test, 0, str.hash);

	/* Test filename with all printable ASCII characters */
	create_qstr(&str, "!@#$%^&*()_+-=[]{}|;':\",./<>?");
	result = hfsplus_hash_dentry(&test_dentry, &str);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_NE(test, 0, str.hash);

	/* Test with embedded null (though not typical for filenames) */
	str.name = "file\0hidden";
	str.len = 11; /* Include the null and text after it */
	str.hash = 0;
	result = hfsplus_hash_dentry(&test_dentry, &str);

	KUNIT_EXPECT_EQ(test, 0, result);
	KUNIT_EXPECT_NE(test, 0, str.hash);
}

/* Test hfsplus_compare_dentry basic functionality */
static void hfsplus_compare_dentry_basic_test(struct kunit *test)
{
	struct qstr name;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test identical strings */
	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "hello", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test different strings - lexicographic order */
	create_qstr(&name, "world");
	result = hfsplus_compare_dentry(&test_dentry, 5, "hello", &name);
	KUNIT_EXPECT_LT(test, result, 0); /* "hello" < "world" */

	result = hfsplus_compare_dentry(&test_dentry, 5, "world", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "world", &name);
	KUNIT_EXPECT_GT(test, result, 0); /* "world" > "hello" */

	/* Test empty strings */
	create_qstr(&name, "");
	result = hfsplus_compare_dentry(&test_dentry, 0, "", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test one empty, one non-empty */
	create_qstr(&name, "test");
	result = hfsplus_compare_dentry(&test_dentry, 0, "", &name);
	KUNIT_EXPECT_LT(test, result, 0); /* "" < "test" */

	create_qstr(&name, "");
	result = hfsplus_compare_dentry(&test_dentry, 4, "test", &name);
	KUNIT_EXPECT_GT(test, result, 0); /* "test" > "" */
}

/* Test case folding behavior in comparison */
static void hfsplus_compare_dentry_casefold_test(struct kunit *test)
{
	struct qstr name;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test with case folding disabled (default) */
	clear_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);

	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "Hello", &name);
	/* Case sensitive: "Hello" != "hello" */
	KUNIT_EXPECT_NE(test, 0, result);

	create_qstr(&name, "Hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "hello", &name);
	/* Case sensitive: "hello" != "Hello" */
	KUNIT_EXPECT_NE(test, 0, result);

	/* Test with case folding enabled */
	set_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);

	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "Hello", &name);
	/* Case insensitive: "Hello" == "hello" */
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&name, "Hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "hello", &name);
	/* Case insensitive: "hello" == "Hello" */
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test mixed case */
	create_qstr(&name, "TeSt");
	result = hfsplus_compare_dentry(&test_dentry, 4, "test", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&name, "test");
	result = hfsplus_compare_dentry(&test_dentry, 4, "TEST", &name);
	KUNIT_EXPECT_EQ(test, 0, result);
}

/* Test special character handling in comparison */
static void hfsplus_compare_dentry_special_chars_test(struct kunit *test)
{
	struct qstr name;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test colon conversion (: becomes /) */
	create_qstr(&name, "file/name");
	result = hfsplus_compare_dentry(&test_dentry, 9, "file:name", &name);
	/* "file:name" == "file/name" after conversion */
	KUNIT_EXPECT_EQ(test, 0, result);

	create_qstr(&name, "file:name");
	result = hfsplus_compare_dentry(&test_dentry, 9, "file/name", &name);
	/* "file/name" == "file:name" after conversion */
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test multiple special characters */
	create_qstr(&name, "///");
	result = hfsplus_compare_dentry(&test_dentry, 3, ":::", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test mixed special and regular characters */
	create_qstr(&name, "a/b:c");
	result = hfsplus_compare_dentry(&test_dentry, 5, "a:b/c", &name);
	/* Both become "a/b/c" after conversion */
	KUNIT_EXPECT_EQ(test, 0, result);
}

/* Test length differences */
static void hfsplus_compare_dentry_length_test(struct kunit *test)
{
	struct qstr name;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test different lengths with common prefix */
	create_qstr(&name, "testing");
	result = hfsplus_compare_dentry(&test_dentry, 4, "test", &name);
	KUNIT_EXPECT_LT(test, result, 0); /* "test" < "testing" */

	create_qstr(&name, "test");
	result = hfsplus_compare_dentry(&test_dentry, 7, "testing", &name);
	KUNIT_EXPECT_GT(test, result, 0); /* "testing" > "test" */

	/* Test exact length match */
	create_qstr(&name, "exact");
	result = hfsplus_compare_dentry(&test_dentry, 5, "exact", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test length parameter vs actual string content */
	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 3, "hel", &name);
	KUNIT_EXPECT_LT(test, result, 0); /* "hel" < "hello" */

	/* Test longer first string but shorter length parameter */
	create_qstr(&name, "hi");
	result = hfsplus_compare_dentry(&test_dentry, 2, "hello", &name);
	/* "he" < "hi" (only first 2 chars compared) */
	KUNIT_EXPECT_LT(test, result, 0);
}

/* Test decomposition flag behavior */
static void hfsplus_compare_dentry_decompose_test(struct kunit *test)
{
	struct qstr name;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test with decomposition disabled (default) */
	clear_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);

	create_qstr(&name, "test");
	result = hfsplus_compare_dentry(&test_dentry, 4, "test", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test with decomposition enabled */
	set_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);

	create_qstr(&name, "test");
	result = hfsplus_compare_dentry(&test_dentry, 4, "test", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* For simple ASCII, decomposition shouldn't affect the result */
	create_qstr(&name, "different");
	result = hfsplus_compare_dentry(&test_dentry, 4, "test", &name);
	KUNIT_EXPECT_NE(test, 0, result);
}

/* Test edge cases and boundary conditions */
static void hfsplus_compare_dentry_edge_cases_test(struct kunit *test)
{
	struct qstr name;
	int result;
	char long_str[256];
	char long_str2[256];
	struct qstr null_name = {
		.name = "a\0b",
		.len = 3,
		.hash = 0
	};

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test very long strings */
	memset(long_str, 'a', sizeof(long_str) - 1);
	long_str[sizeof(long_str) - 1] = '\0';

	create_qstr(&name, long_str);
	result = hfsplus_compare_dentry(&test_dentry, sizeof(long_str) - 1,
					long_str, &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test with difference at the end of long strings */
	memset(long_str2, 'a', sizeof(long_str2) - 1);
	long_str2[sizeof(long_str2) - 1] = '\0';
	long_str2[sizeof(long_str2) - 2] = 'b';
	create_qstr(&name, long_str2);
	result = hfsplus_compare_dentry(&test_dentry, sizeof(long_str) - 1,
					long_str, &name);
	KUNIT_EXPECT_LT(test, result, 0); /* 'a' < 'b' */

	/* Test single character differences */
	create_qstr(&name, "b");
	result = hfsplus_compare_dentry(&test_dentry, 1, "a", &name);
	KUNIT_EXPECT_LT(test, result, 0); /* 'a' < 'b' */

	create_qstr(&name, "a");
	result = hfsplus_compare_dentry(&test_dentry, 1, "b", &name);
	KUNIT_EXPECT_GT(test, result, 0); /* 'b' > 'a' */

	/* Test with null characters in the middle */
	result = hfsplus_compare_dentry(&test_dentry, 3, "a\0b", &null_name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test all printable ASCII characters */
	create_qstr(&name, "!@#$%^&*()");
	result = hfsplus_compare_dentry(&test_dentry, 10, "!@#$%^&*()", &name);
	KUNIT_EXPECT_EQ(test, 0, result);
}

/* Test combined flag behaviors */
static void hfsplus_compare_dentry_combined_flags_test(struct kunit *test)
{
	struct qstr name;
	int result;

	setup_mock_sb();
	setup_mock_dentry();
	test_nls.char2uni = test_char2uni;

	/* Test with both casefold and decompose enabled */
	set_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);
	set_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);

	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "HELLO", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test special chars with case folding */
	create_qstr(&name, "File/Name");
	result = hfsplus_compare_dentry(&test_dentry, 9, "file:name", &name);
	KUNIT_EXPECT_EQ(test, 0, result);

	/* Test with both flags disabled */
	clear_bit(HFSPLUS_SB_CASEFOLD, &test_sb_info.flags);
	clear_bit(HFSPLUS_SB_NODECOMPOSE, &test_sb_info.flags);

	create_qstr(&name, "hello");
	result = hfsplus_compare_dentry(&test_dentry, 5, "HELLO", &name);
	KUNIT_EXPECT_NE(test, 0, result); /* Case sensitive */

	/* But special chars should still be converted */
	create_qstr(&name, "file/name");
	result = hfsplus_compare_dentry(&test_dentry, 9, "file:name", &name);
	KUNIT_EXPECT_EQ(test, 0, result);
}

static struct kunit_case hfsplus_unicode_test_cases[] = {
	KUNIT_CASE(hfsplus_strcasecmp_test),
	KUNIT_CASE(hfsplus_strcmp_test),
	KUNIT_CASE(hfsplus_unicode_edge_cases_test),
	KUNIT_CASE(hfsplus_unicode_boundary_test),
	KUNIT_CASE(hfsplus_uni2asc_basic_test),
	KUNIT_CASE(hfsplus_uni2asc_special_chars_test),
	KUNIT_CASE(hfsplus_uni2asc_buffer_test),
	KUNIT_CASE(hfsplus_uni2asc_corrupted_test),
	KUNIT_CASE(hfsplus_uni2asc_edge_cases_test),
	KUNIT_CASE(hfsplus_asc2uni_basic_test),
	KUNIT_CASE(hfsplus_asc2uni_special_chars_test),
	KUNIT_CASE(hfsplus_asc2uni_buffer_limits_test),
	KUNIT_CASE(hfsplus_asc2uni_edge_cases_test),
	KUNIT_CASE(hfsplus_asc2uni_decompose_test),
	KUNIT_CASE(hfsplus_hash_dentry_basic_test),
	KUNIT_CASE(hfsplus_hash_dentry_casefold_test),
	KUNIT_CASE(hfsplus_hash_dentry_special_chars_test),
	KUNIT_CASE(hfsplus_hash_dentry_decompose_test),
	KUNIT_CASE(hfsplus_hash_dentry_consistency_test),
	KUNIT_CASE(hfsplus_hash_dentry_edge_cases_test),
	KUNIT_CASE(hfsplus_compare_dentry_basic_test),
	KUNIT_CASE(hfsplus_compare_dentry_casefold_test),
	KUNIT_CASE(hfsplus_compare_dentry_special_chars_test),
	KUNIT_CASE(hfsplus_compare_dentry_length_test),
	KUNIT_CASE(hfsplus_compare_dentry_decompose_test),
	KUNIT_CASE(hfsplus_compare_dentry_edge_cases_test),
	KUNIT_CASE(hfsplus_compare_dentry_combined_flags_test),
	{}
};

static struct kunit_suite hfsplus_unicode_test_suite = {
	.name = "hfsplus_unicode",
	.test_cases = hfsplus_unicode_test_cases,
};

kunit_test_suite(hfsplus_unicode_test_suite);

MODULE_DESCRIPTION("KUnit tests for HFS+ Unicode string operations");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
