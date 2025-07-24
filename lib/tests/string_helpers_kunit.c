// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Test cases for lib/string_helpers.c module.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <kunit/test.h>
#include <linux/array_size.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/string_helpers.h>

static void test_string_check_buf(struct kunit *test,
				  const char *name, unsigned int flags,
				  char *in, size_t p,
				  char *out_real, size_t q_real,
				  char *out_test, size_t q_test)
{
	KUNIT_ASSERT_EQ_MSG(test, q_real, q_test, "name:%s", name);
	KUNIT_EXPECT_MEMEQ_MSG(test, out_test, out_real, q_test,
			       "name:%s", name);
}

struct test_string {
	const char *in;
	const char *out;
	unsigned int flags;
};

static const struct test_string strings[] = {
	{
		.in = "\\f\\ \\n\\r\\t\\v",
		.out = "\f\\ \n\r\t\v",
		.flags = UNESCAPE_SPACE,
	},
	{
		.in = "\\40\\1\\387\\0064\\05\\040\\8a\\110\\777",
		.out = " \001\00387\0064\005 \\8aH?7",
		.flags = UNESCAPE_OCTAL,
	},
	{
		.in = "\\xv\\xa\\x2c\\xD\\x6f2",
		.out = "\\xv\n,\ro2",
		.flags = UNESCAPE_HEX,
	},
	{
		.in = "\\h\\\\\\\"\\a\\e\\",
		.out = "\\h\\\"\a\e\\",
		.flags = UNESCAPE_SPECIAL,
	},
};

static void test_string_unescape(struct kunit *test,
				 const char *name, unsigned int flags,
				 bool inplace)
{
	int q_real = 256;
	char *in = kunit_kzalloc(test, q_real, GFP_KERNEL);
	char *out_test = kunit_kzalloc(test, q_real, GFP_KERNEL);
	char *out_real = kunit_kzalloc(test, q_real, GFP_KERNEL);
	int i, p = 0, q_test = 0;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, in);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, out_test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, out_real);

	for (i = 0; i < ARRAY_SIZE(strings); i++) {
		const char *s = strings[i].in;
		int len = strlen(strings[i].in);

		/* Copy string to in buffer */
		memcpy(&in[p], s, len);
		p += len;

		/* Copy expected result for given flags */
		if (flags & strings[i].flags) {
			s = strings[i].out;
			len = strlen(strings[i].out);
		}
		memcpy(&out_test[q_test], s, len);
		q_test += len;
	}
	in[p++] = '\0';

	/* Call string_unescape and compare result */
	if (inplace) {
		memcpy(out_real, in, p);
		if (flags == UNESCAPE_ANY)
			q_real = string_unescape_any_inplace(out_real);
		else
			q_real = string_unescape_inplace(out_real, flags);
	} else if (flags == UNESCAPE_ANY) {
		q_real = string_unescape_any(in, out_real, q_real);
	} else {
		q_real = string_unescape(in, out_real, q_real, flags);
	}

	test_string_check_buf(test, name, flags, in, p - 1, out_real, q_real,
			      out_test, q_test);
}

struct test_string_1 {
	const char *out;
	unsigned int flags;
};

#define	TEST_STRING_2_MAX_S1		32
struct test_string_2 {
	const char *in;
	struct test_string_1 s1[TEST_STRING_2_MAX_S1];
};

#define	TEST_STRING_2_DICT_0		NULL
static const struct test_string_2 escape0[] = {{
	.in = "\f\\ \n\r\t\v",
	.s1 = {{
		.out = "\\f\\ \\n\\r\\t\\v",
		.flags = ESCAPE_SPACE,
	},{
		.out = "\\f\\134\\040\\n\\r\\t\\v",
		.flags = ESCAPE_SPACE | ESCAPE_OCTAL,
	},{
		.out = "\\f\\x5c\\x20\\n\\r\\t\\v",
		.flags = ESCAPE_SPACE | ESCAPE_HEX,
	},{
		/* terminator */
	}}
},{
	.in = "\\h\\\"\a\e\\",
	.s1 = {{
		.out = "\\\\h\\\\\\\"\\a\\e\\\\",
		.flags = ESCAPE_SPECIAL,
	},{
		.out = "\\\\\\150\\\\\\\"\\a\\e\\\\",
		.flags = ESCAPE_SPECIAL | ESCAPE_OCTAL,
	},{
		.out = "\\\\\\x68\\\\\\\"\\a\\e\\\\",
		.flags = ESCAPE_SPECIAL | ESCAPE_HEX,
	},{
		/* terminator */
	}}
},{
	.in = "\eb \\C\007\"\x90\r]",
	.s1 = {{
		.out = "\eb \\C\007\"\x90\\r]",
		.flags = ESCAPE_SPACE,
	},{
		.out = "\\eb \\\\C\\a\\\"\x90\r]",
		.flags = ESCAPE_SPECIAL,
	},{
		.out = "\\eb \\\\C\\a\\\"\x90\\r]",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL,
	},{
		.out = "\\033\\142\\040\\134\\103\\007\\042\\220\\015\\135",
		.flags = ESCAPE_OCTAL,
	},{
		.out = "\\033\\142\\040\\134\\103\\007\\042\\220\\r\\135",
		.flags = ESCAPE_SPACE | ESCAPE_OCTAL,
	},{
		.out = "\\e\\142\\040\\\\\\103\\a\\\"\\220\\015\\135",
		.flags = ESCAPE_SPECIAL | ESCAPE_OCTAL,
	},{
		.out = "\\e\\142\\040\\\\\\103\\a\\\"\\220\\r\\135",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL | ESCAPE_OCTAL,
	},{
		.out = "\eb \\C\007\"\x90\r]",
		.flags = ESCAPE_NP,
	},{
		.out = "\eb \\C\007\"\x90\\r]",
		.flags = ESCAPE_SPACE | ESCAPE_NP,
	},{
		.out = "\\eb \\C\\a\"\x90\r]",
		.flags = ESCAPE_SPECIAL | ESCAPE_NP,
	},{
		.out = "\\eb \\C\\a\"\x90\\r]",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL | ESCAPE_NP,
	},{
		.out = "\\033b \\C\\007\"\\220\\015]",
		.flags = ESCAPE_OCTAL | ESCAPE_NP,
	},{
		.out = "\\033b \\C\\007\"\\220\\r]",
		.flags = ESCAPE_SPACE | ESCAPE_OCTAL | ESCAPE_NP,
	},{
		.out = "\\eb \\C\\a\"\\220\\r]",
		.flags = ESCAPE_SPECIAL | ESCAPE_SPACE | ESCAPE_OCTAL |
			 ESCAPE_NP,
	},{
		.out = "\\x1bb \\C\\x07\"\\x90\\x0d]",
		.flags = ESCAPE_NP | ESCAPE_HEX,
	},{
		/* terminator */
	}}
},{
	.in = "\007 \eb\"\x90\xCF\r",
	.s1 = {{
		.out = "\007 \eb\"\\220\\317\r",
		.flags = ESCAPE_OCTAL | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\\x90\\xcf\r",
		.flags = ESCAPE_HEX | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_NA,
	},{
		/* terminator */
	}}
},{
	/* terminator */
}};

#define	TEST_STRING_2_DICT_1		"b\\ \t\r\xCF"
static const struct test_string_2 escape1[] = {{
	.in = "\f\\ \n\r\t\v",
	.s1 = {{
		.out = "\f\\134\\040\n\\015\\011\v",
		.flags = ESCAPE_OCTAL,
	},{
		.out = "\f\\x5c\\x20\n\\x0d\\x09\v",
		.flags = ESCAPE_HEX,
	},{
		.out = "\f\\134\\040\n\\015\\011\v",
		.flags = ESCAPE_ANY | ESCAPE_APPEND,
	},{
		.out = "\\014\\134\\040\\012\\015\\011\\013",
		.flags = ESCAPE_OCTAL | ESCAPE_APPEND | ESCAPE_NAP,
	},{
		.out = "\\x0c\\x5c\\x20\\x0a\\x0d\\x09\\x0b",
		.flags = ESCAPE_HEX | ESCAPE_APPEND | ESCAPE_NAP,
	},{
		.out = "\f\\134\\040\n\\015\\011\v",
		.flags = ESCAPE_OCTAL | ESCAPE_APPEND | ESCAPE_NA,
	},{
		.out = "\f\\x5c\\x20\n\\x0d\\x09\v",
		.flags = ESCAPE_HEX | ESCAPE_APPEND | ESCAPE_NA,
	},{
		/* terminator */
	}}
},{
	.in = "\\h\\\"\a\xCF\e\\",
	.s1 = {{
		.out = "\\134h\\134\"\a\\317\e\\134",
		.flags = ESCAPE_OCTAL,
	},{
		.out = "\\134h\\134\"\a\\317\e\\134",
		.flags = ESCAPE_ANY | ESCAPE_APPEND,
	},{
		.out = "\\134h\\134\"\\007\\317\\033\\134",
		.flags = ESCAPE_OCTAL | ESCAPE_APPEND | ESCAPE_NAP,
	},{
		.out = "\\134h\\134\"\a\\317\e\\134",
		.flags = ESCAPE_OCTAL | ESCAPE_APPEND | ESCAPE_NA,
	},{
		/* terminator */
	}}
},{
	.in = "\eb \\C\007\"\x90\r]",
	.s1 = {{
		.out = "\e\\142\\040\\134C\007\"\x90\\015]",
		.flags = ESCAPE_OCTAL,
	},{
		/* terminator */
	}}
},{
	.in = "\007 \eb\"\x90\xCF\r",
	.s1 = {{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_SPACE | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_SPECIAL | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\317\r",
		.flags = ESCAPE_OCTAL | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\317\r",
		.flags = ESCAPE_SPACE | ESCAPE_OCTAL | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\317\r",
		.flags = ESCAPE_SPECIAL | ESCAPE_OCTAL | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\317\r",
		.flags = ESCAPE_ANY | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\xcf\r",
		.flags = ESCAPE_HEX | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\xcf\r",
		.flags = ESCAPE_SPACE | ESCAPE_HEX | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\xcf\r",
		.flags = ESCAPE_SPECIAL | ESCAPE_HEX | ESCAPE_NA,
	},{
		.out = "\007 \eb\"\x90\\xcf\r",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL | ESCAPE_HEX | ESCAPE_NA,
	},{
		/* terminator */
	}}
},{
	.in = "\007 \eb\"\x90\xCF\r",
	.s1 = {{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\xCF\\r",
		.flags = ESCAPE_SPACE | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\xCF\r",
		.flags = ESCAPE_SPECIAL | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\xCF\\r",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\317\\015",
		.flags = ESCAPE_OCTAL | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\317\\r",
		.flags = ESCAPE_SPACE | ESCAPE_OCTAL | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\317\\015",
		.flags = ESCAPE_SPECIAL | ESCAPE_OCTAL | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\317\r",
		.flags = ESCAPE_ANY | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\xcf\\x0d",
		.flags = ESCAPE_HEX | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\xcf\\r",
		.flags = ESCAPE_SPACE | ESCAPE_HEX | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\xcf\\x0d",
		.flags = ESCAPE_SPECIAL | ESCAPE_HEX | ESCAPE_NAP,
	},{
		.out = "\007 \eb\"\x90\\xcf\\r",
		.flags = ESCAPE_SPACE | ESCAPE_SPECIAL | ESCAPE_HEX | ESCAPE_NAP,
	},{
		/* terminator */
	}}
},{
	/* terminator */
}};

static const struct test_string strings_upper[] = {
	{
		.in = "abcdefgh1234567890test",
		.out = "ABCDEFGH1234567890TEST",
	},
	{
		.in = "abCdeFgH1234567890TesT",
		.out = "ABCDEFGH1234567890TEST",
	},
};

static const struct test_string strings_lower[] = {
	{
		.in = "ABCDEFGH1234567890TEST",
		.out = "abcdefgh1234567890test",
	},
	{
		.in = "abCdeFgH1234567890TesT",
		.out = "abcdefgh1234567890test",
	},
};

static const char *test_string_find_match(const struct test_string_2 *s2,
					  unsigned int flags)
{
	const struct test_string_1 *s1 = s2->s1;
	unsigned int i;

	if (!flags)
		return s2->in;

	/* Test cases are NULL-aware */
	flags &= ~ESCAPE_NULL;

	/* ESCAPE_OCTAL has a higher priority */
	if (flags & ESCAPE_OCTAL)
		flags &= ~ESCAPE_HEX;

	for (i = 0; i < TEST_STRING_2_MAX_S1 && s1->out; i++, s1++)
		if (s1->flags == flags)
			return s1->out;
	return NULL;
}

static void
test_string_escape_overflow(struct kunit *test,
			    const char *in, int p, unsigned int flags, const char *esc,
			    int q_test, const char *name)
{
	int q_real;

	q_real = string_escape_mem(in, p, NULL, 0, flags, esc);
	KUNIT_EXPECT_EQ_MSG(test, q_real, q_test, "name:%s: flags:%#x", name, flags);
}

static void test_string_escape(struct kunit *test, const char *name,
			       const struct test_string_2 *s2,
			       unsigned int flags, const char *esc)
{
	size_t out_size = 512;
	char *out_test = kunit_kzalloc(test, out_size, GFP_KERNEL);
	char *out_real = kunit_kzalloc(test, out_size, GFP_KERNEL);
	char *in = kunit_kzalloc(test, 256, GFP_KERNEL);
	int p = 0, q_test = 0;
	int q_real;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, out_test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, out_real);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, in);

	for (; s2->in; s2++) {
		const char *out;
		int len;

		/* NULL injection */
		if (flags & ESCAPE_NULL) {
			in[p++] = '\0';
			/* '\0' passes isascii() test */
			if (flags & ESCAPE_NA && !(flags & ESCAPE_APPEND && esc)) {
				out_test[q_test++] = '\0';
			} else {
				out_test[q_test++] = '\\';
				out_test[q_test++] = '0';
			}
		}

		/* Don't try strings that have no output */
		out = test_string_find_match(s2, flags);
		if (!out)
			continue;

		/* Copy string to in buffer */
		len = strlen(s2->in);
		memcpy(&in[p], s2->in, len);
		p += len;

		/* Copy expected result for given flags */
		len = strlen(out);
		memcpy(&out_test[q_test], out, len);
		q_test += len;
	}

	q_real = string_escape_mem(in, p, out_real, out_size, flags, esc);

	test_string_check_buf(test, name, flags, in, p, out_real, q_real, out_test,
			      q_test);

	test_string_escape_overflow(test, in, p, flags, esc, q_test, name);
}

#define string_get_size_maxbuf 16
#define test_string_get_size_one(size, blk_size, exp_result10, exp_result2)      \
	do {                                                                     \
		BUILD_BUG_ON(sizeof(exp_result10) >= string_get_size_maxbuf);    \
		BUILD_BUG_ON(sizeof(exp_result2) >= string_get_size_maxbuf);     \
		__test_string_get_size(test, (size), (blk_size), (exp_result10), \
				       (exp_result2));                           \
	} while (0)


static void test_string_get_size_check(struct kunit *test,
				       const char *units,
				       const char *exp,
				       char *res,
				       const u64 size,
				       const u64 blk_size)
{
	KUNIT_EXPECT_MEMEQ_MSG(test, res, exp, strlen(exp) + 1,
		"string_get_size(size = %llu, blk_size = %llu, units = %s)",
		size, blk_size, units);
}

static void __strchrcut(char *dst, const char *src, const char *cut)
{
	const char *from = src;
	size_t len;

	do {
		len = strcspn(from, cut);
		memcpy(dst, from, len);
		dst += len;
		from += len;
	} while (*from++);
	*dst = '\0';
}

static void __test_string_get_size_one(struct kunit *test,
				       const u64 size, const u64 blk_size,
				       const char *exp_result10,
				       const char *exp_result2,
				       enum string_size_units units,
				       const char *cut)
{
	char buf10[string_get_size_maxbuf];
	char buf2[string_get_size_maxbuf];
	char exp10[string_get_size_maxbuf];
	char exp2[string_get_size_maxbuf];
	char prefix10[64];
	char prefix2[64];

	sprintf(prefix10, "STRING_UNITS_10 [%s]", cut);
	sprintf(prefix2, "STRING_UNITS_2 [%s]", cut);

	__strchrcut(exp10, exp_result10, cut);
	__strchrcut(exp2, exp_result2, cut);

	string_get_size(size, blk_size, STRING_UNITS_10 | units, buf10, sizeof(buf10));
	string_get_size(size, blk_size, STRING_UNITS_2 | units, buf2, sizeof(buf2));

	test_string_get_size_check(test, prefix10, exp10, buf10, size, blk_size);
	test_string_get_size_check(test, prefix2, exp2, buf2, size, blk_size);
}

static void __test_string_get_size(struct kunit *test,
				   const u64 size, const u64 blk_size,
				   const char *exp_result10,
				   const char *exp_result2)
{
	struct {
		enum string_size_units units;
		const char *cut;
	} get_size_test_cases[] = {
		{ 0, "" },
		{ STRING_UNITS_NO_SPACE, " " },
		{ STRING_UNITS_NO_SPACE | STRING_UNITS_NO_BYTES, " B" },
		{ STRING_UNITS_NO_BYTES, "B" },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(get_size_test_cases); i++)
		__test_string_get_size_one(test, size, blk_size,
					   exp_result10, exp_result2,
					   get_size_test_cases[i].units,
					   get_size_test_cases[i].cut);
}

static void test_get_size(struct kunit *test)
{
	/* small values */
	test_string_get_size_one(0, 512, "0 B", "0 B");
	test_string_get_size_one(1, 512, "512 B", "512 B");
	test_string_get_size_one(1100, 1, "1.10 kB", "1.07 KiB");

	/* normal values */
	test_string_get_size_one(16384, 512, "8.39 MB", "8.00 MiB");
	test_string_get_size_one(500118192, 512, "256 GB", "238 GiB");
	test_string_get_size_one(8192, 4096, "33.6 MB", "32.0 MiB");

	/* weird block sizes */
	test_string_get_size_one(3000, 1900, "5.70 MB", "5.44 MiB");

	/* huge values */
	test_string_get_size_one(U64_MAX, 4096, "75.6 ZB", "64.0 ZiB");
	test_string_get_size_one(4096, U64_MAX, "75.6 ZB", "64.0 ZiB");
}

static void test_upper_lower(struct kunit *test)
{
	char *dst;
	int i;

	for (i = 0; i < ARRAY_SIZE(strings_upper); i++) {
		const char *s = strings_upper[i].in;
		int len = strlen(strings_upper[i].in) + 1;

		dst = kmalloc(len, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, dst);

		string_upper(dst, s);
		KUNIT_EXPECT_STREQ(test, dst, strings_upper[i].out);
		kfree(dst);
	}

	for (i = 0; i < ARRAY_SIZE(strings_lower); i++) {
		const char *s = strings_lower[i].in;
		int len = strlen(strings_lower[i].in) + 1;

		dst = kmalloc(len, GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, dst);

		string_lower(dst, s);
		KUNIT_EXPECT_STREQ(test, dst, strings_lower[i].out);
		kfree(dst);
	}
}

static void test_unescape(struct kunit *test)
{
	unsigned int i;

	for (i = 0; i < UNESCAPE_ALL_MASK + 1; i++)
		test_string_unescape(test, "unescape", i, false);
	test_string_unescape(test, "unescape inplace",
			     get_random_u32_below(UNESCAPE_ALL_MASK + 1), true);

	/* Without dictionary */
	for (i = 0; i < ESCAPE_ALL_MASK + 1; i++)
		test_string_escape(test, "escape 0", escape0, i, TEST_STRING_2_DICT_0);

	/* With dictionary */
	for (i = 0; i < ESCAPE_ALL_MASK + 1; i++)
		test_string_escape(test, "escape 1", escape1, i, TEST_STRING_2_DICT_1);
}

/* Test structure for memcpy_and_pad structure copying tests */
struct test_struct {
	u8 byte1;
	u16 word1;
	u8 byte2;
	u32 dword1;
	u8 byte3;
} __packed;

static void test_memcpy_and_pad(struct kunit *test)
{
	char dest[20];
	const char *src = "hello";
	struct test_struct struct_dest[3];
	const struct test_struct struct_src = {
		.byte1 = 0xAA,
		.word1 = 0xBBCC,
		.byte2 = 0xDD,
		.dword1 = 0xEEFF1122,
		.byte3 = 0x33
	};
	u8 *struct_bytes;
	int i;

	/* Test 1: Basic functionality - dest_len > count */
	memset(dest, 0xFF, sizeof(dest));  /* Fill with known pattern */
	memcpy_and_pad(dest, 10, src, 5, 0x11);
	KUNIT_EXPECT_MEMEQ(test, dest, "hello", 5);
	/* Check padding */
	for (i = 5; i < 10; i++)
		KUNIT_EXPECT_EQ(test, (unsigned char)dest[i], 0x11);
	/* Ensure we didn't write past dest_len */
	KUNIT_EXPECT_EQ(test, (unsigned char)dest[10], 0xFF);

	/* Test 2: dest_len == count */
	memset(dest, 0xFF, sizeof(dest));
	memcpy_and_pad(dest, 5, src, 5, 0x22);
	KUNIT_EXPECT_MEMEQ(test, dest, "hello", 5);
	/* No padding should occur */
	KUNIT_EXPECT_EQ(test, (unsigned char)dest[5], 0xFF);

	/* Test 3: dest_len < count (truncation) */
	memset(dest, 0xFF, sizeof(dest));
	memcpy_and_pad(dest, 3, src, 5, 0x33);
	KUNIT_EXPECT_MEMEQ(test, dest, "hel", 3);
	/* No padding should occur, and nothing beyond dest_len should be touched */
	KUNIT_EXPECT_EQ(test, (unsigned char)dest[3], 0xFF);

	/* Test 4: Zero count */
	memset(dest, 0xFF, sizeof(dest));
	memcpy_and_pad(dest, 5, src, 0, 0x44);
	/* Should be all padding */
	for (i = 0; i < 5; i++)
		KUNIT_EXPECT_EQ(test, (unsigned char)dest[i], 0x44);
	KUNIT_EXPECT_EQ(test, (unsigned char)dest[5], 0xFF);

	/* Test 5: Zero dest_len */
	memset(dest, 0xFF, sizeof(dest));
	memcpy_and_pad(dest, 0, src, 5, 0x55);
	/* Should do nothing */
	KUNIT_EXPECT_EQ(test, (unsigned char)dest[0], 0xFF);

	/* Test 6: Different padding characters */
	memset(dest, 0xFF, sizeof(dest));
	memcpy_and_pad(dest, 8, "hi", 2, '\0');
	KUNIT_EXPECT_MEMEQ(test, dest, "hi", 2);
	for (i = 2; i < 8; i++)
		KUNIT_EXPECT_EQ(test, dest[i], '\0');

	/* Test 7: Negative pad value (should work as unsigned char) */
	memset(dest, 0x00, sizeof(dest));
	memcpy_and_pad(dest, 5, "a", 1, -1);
	KUNIT_EXPECT_EQ(test, dest[0], 'a');
	for (i = 1; i < 5; i++)
		KUNIT_EXPECT_EQ(test, (unsigned char)dest[i], 0xFF);

	/* Test 8: Single byte operations */
	memset(dest, 0x00, sizeof(dest));
	memcpy_and_pad(dest, 1, "X", 1, 0x88);
	KUNIT_EXPECT_EQ(test, dest[0], 'X');
	KUNIT_EXPECT_EQ(test, (unsigned char)dest[1], 0x00);

	/* Test 9: Large pad scenario */
	memset(dest, 0x00, sizeof(dest));
	memcpy_and_pad(dest, 15, "test", 4, 0x99);
	KUNIT_EXPECT_MEMEQ(test, dest, "test", 4);
	for (i = 4; i < 15; i++)
		KUNIT_EXPECT_EQ(test, (unsigned char)dest[i], 0x99);

	/* Test 10: Structure copying - full copy with padding */
	memset(struct_dest, 0xFF, sizeof(struct_dest));
	memcpy_and_pad(struct_dest, sizeof(struct_dest), &struct_src, sizeof(struct_src), 0x55);

	/* Verify first struct copied correctly */
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte1, struct_src.byte1);
	KUNIT_EXPECT_EQ(test, struct_dest[0].word1, struct_src.word1);
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte2, struct_src.byte2);
	KUNIT_EXPECT_EQ(test, struct_dest[0].dword1, struct_src.dword1);
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte3, struct_src.byte3);

	/* Verify padding area - remaining struct elements should be filled with 0x55 */
	struct_bytes = (u8 *)&struct_dest[1];
	for (i = 0; i < sizeof(struct_dest) - sizeof(struct_src); i++)
		KUNIT_EXPECT_EQ(test, struct_bytes[i], 0x55);

	/* Test 11: Structure copying - partial copy with padding */
	memset(struct_dest, 0xFF, sizeof(struct_dest));
	memcpy_and_pad(struct_dest, sizeof(struct_src) * 2, &struct_src,
		       sizeof(struct_src) / 2, 0x77);

	/* Verify partial struct data copied (first 4 bytes of 9-byte struct) */
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte1, struct_src.byte1);   /* offset 0: copied */
	KUNIT_EXPECT_EQ(test, struct_dest[0].word1, struct_src.word1);   /* offset 1-2: copied */
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte2, struct_src.byte2);   /* offset 3: copied */
	/* offset 4 and beyond should be padding since only 4 bytes were copied */
	KUNIT_EXPECT_EQ(test, struct_dest[0].dword1, 0x77777777);
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte3, 0x77);

	/* Second struct should be entirely padding */
	KUNIT_EXPECT_EQ(test, struct_dest[1].byte1, 0x77);
	KUNIT_EXPECT_EQ(test, struct_dest[1].word1, 0x7777);
	KUNIT_EXPECT_EQ(test, struct_dest[1].byte2, 0x77);
	KUNIT_EXPECT_EQ(test, struct_dest[1].dword1, 0x77777777);
	KUNIT_EXPECT_EQ(test, struct_dest[1].byte3, 0x77);

	/* Test 12: Structure copying - truncation (dest_len < count, no padding) */
	memset(struct_dest, 0xFF, sizeof(struct_dest));
	memcpy_and_pad(struct_dest, sizeof(struct_src) - 2, &struct_src, sizeof(struct_src), 0x99);

	/* Verify truncated data - only first 7 bytes of 9-byte struct copied */
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte1, struct_src.byte1);    /* offset 0: copied */
	KUNIT_EXPECT_EQ(test, struct_dest[0].word1, struct_src.word1);    /* offset 1-2: copied */
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte2, struct_src.byte2);    /* offset 3: copied */
	/* offset 4-7: dword1 partially copied (first 3 bytes), last byte remains 0xFF */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	KUNIT_EXPECT_EQ(test, struct_dest[0].dword1, 0xFFFF1122); /* LE: 22 11 FF FF */
#else
	KUNIT_EXPECT_EQ(test, struct_dest[0].dword1, 0xEEFF11FF); /* BE: EE FF 11 FF */
#endif
	/* byte3 should remain as initialization since truncated at 7 bytes */
	KUNIT_EXPECT_EQ(test, struct_dest[0].byte3, 0xFF);

	/* Remaining struct elements should be untouched */
	KUNIT_EXPECT_EQ(test, struct_dest[1].byte1, 0xFF);
	KUNIT_EXPECT_EQ(test, struct_dest[1].word1, 0xFFFF);
	KUNIT_EXPECT_EQ(test, struct_dest[1].byte2, 0xFF);
	KUNIT_EXPECT_EQ(test, struct_dest[1].dword1, 0xFFFFFFFF);
	KUNIT_EXPECT_EQ(test, struct_dest[1].byte3, 0xFF);
	KUNIT_EXPECT_EQ(test, struct_dest[2].byte1, 0xFF);
	KUNIT_EXPECT_EQ(test, struct_dest[2].word1, 0xFFFF);
	KUNIT_EXPECT_EQ(test, struct_dest[2].byte2, 0xFF);
	KUNIT_EXPECT_EQ(test, struct_dest[2].dword1, 0xFFFFFFFF);
	KUNIT_EXPECT_EQ(test, struct_dest[2].byte3, 0xFF);
}

static struct kunit_case string_helpers_test_cases[] = {
	KUNIT_CASE(test_get_size),
	KUNIT_CASE(test_upper_lower),
	KUNIT_CASE(test_unescape),
	KUNIT_CASE(test_memcpy_and_pad),
	{}
};

static struct kunit_suite string_helpers_test_suite = {
	.name = "string_helpers",
	.test_cases = string_helpers_test_cases,
};

kunit_test_suites(&string_helpers_test_suite);

MODULE_DESCRIPTION("Test cases for string helpers module");
MODULE_LICENSE("Dual BSD/GPL");
