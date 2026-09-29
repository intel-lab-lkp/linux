// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>
#include <linux/pgtable.h>

struct ptval_str {
	u64 val;
	const char *str;
};

static struct ptval_str ptval_u32_tests[] = {
	{ .val = 0x1234abcd,	.str = "1234abcd"},
	{ .val = 0x4455aabb,	.str = "4455aabb"},
	{ .val = 0xccddeeff,	.str = "ccddeeff"},
	{ .val = 0x12345678,	.str = "12345678"},
	{ .val = 0x87654321,	.str = "87654321"},
	{ .val = 0xaabbccdd,	.str = "aabbccdd"},
	{ .val = 0xaa44cc66,	.str = "aa44cc66"},
	{ .val = 0x11442266,	.str = "11442266"},
};

static struct ptval_str ptval_u64_tests[] = {
	{ .val = 0x123456789abcdef0ULL,	.str = "123456789abcdef0"},
	{ .val = 0x113355779abcdef0ULL,	.str = "113355779abcdef0"},
	{ .val = 0xaabbccddeeffaabbULL,	.str = "aabbccddeeffaabb"},
	{ .val = 0x1234567812345678ULL,	.str = "1234567812345678"},
	{ .val = 0x0000000000000000ULL,	.str = "0000000000000000"},
	{ .val = 0xffffffffffffffffULL,	.str = "ffffffffffffffff"},
	{ .val = 0xaa00cc00ee00aa00ULL,	.str = "aa00cc00ee00aa00"},
	{ .val = 0xc0ffeec0ffeedeadULL,	.str = "c0ffeec0ffeedead"},
};

static void ptval_hex_str_u32(struct kunit *test)
{
	char buf[PTVAL_STR_MAX];
	int i;

	for (i = 0; i < ARRAY_SIZE(ptval_u32_tests); i++) {
		u32 val = ptval_u32_tests[i].val;

		ptval_bytes_to_hex_str(buf, sizeof(buf), &val, sizeof(val));
		KUNIT_EXPECT_STREQ(test, buf, ptval_u32_tests[i].str);
	}
}

static void ptval_hex_str_u64(struct kunit *test)
{
	char buf[PTVAL_STR_MAX];
	int i;

	for (i = 0; i < ARRAY_SIZE(ptval_u64_tests); i++) {
		u64 val = ptval_u64_tests[i].val;

		ptval_bytes_to_hex_str(buf, sizeof(buf), &val, sizeof(val));
		KUNIT_EXPECT_STREQ(test, buf, ptval_u64_tests[i].str);
	}
}

#ifdef __SIZEOF_INT128__
struct ptval_128_str {
	u128 val;
	const char *str;
};

static struct ptval_128_str ptval_u128_tests[] = {
	{ .val = (u128)0x1122334455667788 << 64 |
		 (u128)0x123456789abcdef0, .str = "1122334455667788123456789abcdef0"},
	{ .val = (u128)0xaabbccddeeffaabb << 64 |
		 (u128)0xaabbccddeeffaabb, .str = "aabbccddeeffaabbaabbccddeeffaabb"},
	{ .val = (u128)0x0000000000000000 << 64 |
		 (u128)0x0000000000000000, .str = "00000000000000000000000000000000"},
	{ .val = (u128)0xffffffffffffffff << 64 |
		 (u128)0xffffffffffffffff, .str = "ffffffffffffffffffffffffffffffff"},
	{ .val = (u128)0x1234123412341234 << 64 |
		 (u128)0xabcdabcdabcdabcd, .str = "1234123412341234abcdabcdabcdabcd"},
	{ .val = (u128)0xc0ffeec0ffeedead << 64 |
		 (u128)0xc0ffeec0ffeedead, .str = "c0ffeec0ffeedeadc0ffeec0ffeedead"},
	{ .val = (u128)0xa5a5a5a5a55a5a5a << 64 |
		 (u128)0xa5a5a5a55a5a5a5a, .str = "a5a5a5a5a55a5a5aa5a5a5a5a55a5a5a"},
	{ .val = (u128)0x4455445544554455 << 64 |
		 (u128)0x2233223322332233, .str = "44554455445544552233223322332233"},
};

static void ptval_hex_str_u128(struct kunit *test)
{
	char buf[PTVAL_STR_MAX];
	int i;

	for (i = 0; i < ARRAY_SIZE(ptval_u128_tests); i++) {
		u128 val = ptval_u128_tests[i].val;

		ptval_bytes_to_hex_str(buf, sizeof(buf), &val, sizeof(val));
		KUNIT_EXPECT_STREQ(test, buf, ptval_u128_tests[i].str);
	}
}
#endif

static struct kunit_case ptval_hex_str_tests[] = {
	KUNIT_CASE(ptval_hex_str_u32),
	KUNIT_CASE(ptval_hex_str_u64),
#ifdef __SIZEOF_INT128__
	KUNIT_CASE(ptval_hex_str_u128),
#endif
	{}
};

static struct kunit_suite ptval_hex_str_test_suite = {
	.name = "ptval-hex-str",
	.test_cases = ptval_hex_str_tests,
};
kunit_test_suite(ptval_hex_str_test_suite);
