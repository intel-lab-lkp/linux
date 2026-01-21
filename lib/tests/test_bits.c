// SPDX-License-Identifier: GPL-2.0+
/*
 * Test cases for functions and macros in bits.h
 */

#define KBUILD_EXTRA_WARNc 1

#include <kunit/test.h>
#include <linux/bits.h>
#include <linux/types.h>

#define assert_type(t, x) _Generic(x, t: x, default: 0)

static_assert(assert_type(unsigned int, BIT_U8(0)) == 1u);
static_assert(assert_type(unsigned int, BIT_U16(0)) == 1u);
static_assert(assert_type(u32, BIT_U32(0)) == 1u);
static_assert(assert_type(u64, BIT_U64(0)) == 1ull);

static_assert(assert_type(unsigned int, BIT_U8(7)) == 0x80u);
static_assert(assert_type(unsigned int, BIT_U16(15)) == 0x8000u);
static_assert(assert_type(u32, BIT_U32(31)) == 0x80000000u);
static_assert(assert_type(u64, BIT_U64(63)) == 0x8000000000000000ull);

static_assert(assert_type(unsigned long, GENMASK(31, 0)) == U32_MAX);
static_assert(assert_type(unsigned long long, GENMASK_ULL(63, 0)) == U64_MAX);
static_assert(assert_type(unsigned int, GENMASK_U8(7, 0)) == U8_MAX);
static_assert(assert_type(unsigned int, GENMASK_U16(15, 0)) == U16_MAX);
static_assert(assert_type(u32, GENMASK_U32(31, 0)) == U32_MAX);
static_assert(assert_type(u64, GENMASK_U64(63, 0)) == U64_MAX);

/* FIXME: add a test case written in asm for GENMASK() and GENMASK_ULL() */

static void __genmask_test(struct kunit *test)
{
	BUILD_BUG_ON(__GENMASK(0, 0) != 1ul);
	BUILD_BUG_ON(__GENMASK(1, 0) != 3ul);
	BUILD_BUG_ON(__GENMASK(2, 1) != 6ul);
	BUILD_BUG_ON(__GENMASK(31, 0) != 0xFFFFFFFFul);
}

static void __genmask_ull_test(struct kunit *test)
{
	BUILD_BUG_ON(__GENMASK_ULL(0, 0) != 1ull);
	BUILD_BUG_ON(__GENMASK_ULL(1, 0) != 3ull);
	BUILD_BUG_ON(__GENMASK_ULL(39, 21) != 0x000000ffffe00000ull);
	BUILD_BUG_ON(__GENMASK_ULL(63, 0) != 0xffffffffffffffffull);
}

static void genmask_test(struct kunit *test)
{
	BUILD_BUG_ON(GENMASK(0, 0) != 1ul);
	BUILD_BUG_ON(GENMASK(1, 0) != 3ul);
	BUILD_BUG_ON(GENMASK(2, 1) != 6ul);
	BUILD_BUG_ON(GENMASK(31, 0) != 0xFFFFFFFFul);

	BUILD_BUG_ON(GENMASK_U8(0, 0) != 1u);
	BUILD_BUG_ON(GENMASK_U16(1, 0) != 3u);
	BUILD_BUG_ON(GENMASK_U32(16, 16) != 0x10000);

#ifdef TEST_GENMASK_FAILURES
	/* these should fail compilation */
	GENMASK(0, 1);
	GENMASK(0, 10);
	GENMASK(9, 10);

	GENMASK_U32(0, 31);
	GENMASK_U64(64, 0);
	GENMASK_U32(32, 0);
	GENMASK_U16(16, 0);
	GENMASK_U8(8, 0);
#endif


}

static void genmask_ull_test(struct kunit *test)
{
	BUILD_BUG_ON(GENMASK_ULL(0, 0) != 1ull);
	BUILD_BUG_ON(GENMASK_ULL(1, 0) != 3ull);
	BUILD_BUG_ON(GENMASK_ULL(39, 21) != 0x000000ffffe00000ull);
	BUILD_BUG_ON(GENMASK_ULL(63, 0) != 0xffffffffffffffffull);

#ifdef TEST_GENMASK_FAILURES
	/* these should fail compilation */
	GENMASK_ULL(0, 1);
	GENMASK_ULL(0, 10);
	GENMASK_ULL(9, 10);
#endif
}

static void genmask_u128_test(struct kunit *test)
{
#ifdef CONFIG_ARCH_SUPPORTS_INT128
	/* Below 64 bit masks */
	BUILD_BUG_ON(GENMASK_U128(0, 0) != 0x0000000000000001ull);
	BUILD_BUG_ON(GENMASK_U128(1, 0) != 0x0000000000000003ull);
	BUILD_BUG_ON(GENMASK_U128(2, 1) != 0x0000000000000006ull);
	BUILD_BUG_ON(GENMASK_U128(31, 0) != 0x00000000ffffffffull);
	BUILD_BUG_ON(GENMASK_U128(39, 21) != 0x000000ffffe00000ull);
	BUILD_BUG_ON(GENMASK_U128(63, 0) != 0xffffffffffffffffull);

	/* Above 64 bit masks - only 64 bit portion can be validated once */
	BUILD_BUG_ON(GENMASK_U128(64, 0) >> 1 != 0xffffffffffffffffull);
	BUILD_BUG_ON(GENMASK_U128(81, 50) >> 50 != 0x00000000ffffffffull);
	BUILD_BUG_ON(GENMASK_U128(87, 64) >> 64 != 0x0000000000ffffffull);
	BUILD_BUG_ON(GENMASK_U128(87, 80) >> 64 != 0x0000000000ff0000ull);

	BUILD_BUG_ON(GENMASK_U128(127, 0) >> 64 != 0xffffffffffffffffull);
	BUILD_BUG_ON((u64)GENMASK_U128(127, 0) != 0xffffffffffffffffull);
	BUILD_BUG_ON(GENMASK_U128(127, 126) >> 126 != 0x0000000000000003ull);
	BUILD_BUG_ON(GENMASK_U128(127, 127) >> 127 != 0x0000000000000001ull);
#ifdef TEST_GENMASK_FAILURES
	/* these should fail compilation */
	GENMASK_U128(0, 1);
	GENMASK_U128(0, 10);
	GENMASK_U128(9, 10);
#endif /* TEST_GENMASK_FAILURES */
#endif /* CONFIG_ARCH_SUPPORTS_INT128 */
}

static void genmask_input_check_test(struct kunit *test)
{
	unsigned int x = 1, y = 2;
	int z = 1, w = 2;

	OPTIMIZER_HIDE_VAR(x);
	OPTIMIZER_HIDE_VAR(y);
	OPTIMIZER_HIDE_VAR(z);
	OPTIMIZER_HIDE_VAR(w);

	/* Unknown input */
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(x, 0, 32) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(0, x, 32) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(x, y, 32) != 0);

	BUILD_BUG_ON(GENMASK_INPUT_CHECK(z, 0, 32) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(0, z, 32) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(z, w, 32) != 0);

	/* Valid input */
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(1, 1, 32) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(39, 21, 64) != 0);

	BUILD_BUG_ON(GENMASK_INPUT_CHECK(100, 80, 128) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(110, 65, 128) != 0);
	BUILD_BUG_ON(GENMASK_INPUT_CHECK(127, 0, 128) != 0);
}


static struct kunit_case bits_test_cases[] = {
	KUNIT_CASE(__genmask_test),
	KUNIT_CASE(__genmask_ull_test),
	KUNIT_CASE(genmask_test),
	KUNIT_CASE(genmask_ull_test),
	KUNIT_CASE(genmask_u128_test),
	KUNIT_CASE(genmask_input_check_test),
	{}
};

static struct kunit_suite bits_test_suite = {
	.name = "bits-test",
	.test_cases = bits_test_cases,
};
kunit_test_suite(bits_test_suite);

MODULE_DESCRIPTION("Test cases for functions and macros in bits.h");
MODULE_LICENSE("GPL");
