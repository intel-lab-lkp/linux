// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 Intel Corporation */

#include <kunit/test.h>

#include "idpf.h"
#include "idpf_ptp.h"

#if IS_ENABLED(CONFIG_PTP_1588_CLOCK)

struct pkb_tstamp_test_case {
	const char *desc;
	u64 cached_phc_time;
	u32 in_timestamp;
	u8  gran;
	u64 expected;
};

struct pkb_tstamp_negative_case {
	const char *desc;
	u64 cached_phc_time;
	u64 current_phc_time;
	u8 gran;
	u64 expected;
};

/*
 * KUnit tests for idpf_pkb_tstamp_extend_23b_to_64b().
 *
 * The function extends a raw 23-bit Packet Builder (PKB) timestamp to a
 * full 64-bit nanosecond value using the cached PHC time.  @gran is a
 * left-shift that converts the 23-bit raw counter value to nanoseconds:
 * one hardware tick equals 2^gran nanoseconds (e.g. gran=7 - 128 ns/tick,
 * gran=12 - 4096 ns/tick).
 */
static const struct pkb_tstamp_test_case pkb_tstamp_cases[] = {
	{
		.desc		 = "gran = 7: same bucket rounds down",
		.gran		 = 7,
		.cached_phc_time = 0x25,
		.in_timestamp	 = 0x0,
		.expected	 = 0x0,
	},
	{
		.desc		 = "gran = 7: reverse small delta",
		.gran		 = 7,
		.cached_phc_time = 0x85,
		.in_timestamp	 = 0x1,
		.expected	 = 0x80,
	},
	{
		.desc		 = "gran = 7: forward small delta",
		.gran		 = 7,
		.cached_phc_time = 0x80,
		.in_timestamp	 = 0x2,
		.expected	 = 0x100,
	},
	{
		.desc		 = "gran = 7: forward wrap across 30-bit ring",
		.gran		 = 7,
		.cached_phc_time = 0x123fffff80,
		.in_timestamp	 = 0x0,
		.expected	 = 0x1240000000,
	},
	{
		.desc		 = "gran = 7: reverse wrap across 30-bit ring",
		.gran		 = 7,
		.cached_phc_time = 0x1240000000,
		.in_timestamp	 = 0x7fffff,
		.expected	 = 0x123fffff80,
	},
	{
		.desc		 = "gran = 7: delta == half-range boundary",
		.gran		 = 7,
		.cached_phc_time = 0x1220000080,
		.in_timestamp	 = 0x1,
		.expected	 = 0x1200000080,
	},
	{
		.desc		 = "gran = 9: reverse small delta",
		.gran		 = 9,
		.cached_phc_time = 0x255,
		.in_timestamp	 = 0x1,
		.expected	 = 0x200,
	},
	{
		.desc		 = "gran = 9: upper raw bits are masked",
		.gran		 = 9,
		.cached_phc_time = 0x255,
		.in_timestamp	 = 0xFF800001,
		.expected	 = 0x200,
	},
	{
		.desc		 = "gran = 9: forward wrap across 32-bit ring",
		.gran		 = 9,
		.cached_phc_time = 0x00000001fffffff0,
		.in_timestamp	 = 0x1,
		.expected	 = 0x0000000200000200,
	},
	{
		.desc		 = "gran = 9: reverse wrap across 32-bit ring",
		.gran		 = 9,
		.cached_phc_time = 0x0000000200000200,
		.in_timestamp	 = 0x7fffff,
		.expected	 = 0x00000001fffffe00,
	},
	{
		.desc		 = "gran = 9: delta == half-range boundary",
		.gran		 = 9,
		.cached_phc_time = 0x0000000280000200,
		.in_timestamp	 = 0x1,
		.expected	 = 0x0000000200000200,
	},
	{
		.desc		 = "gran = 12: same bucket rounds down",
		.gran		 = 12,
		.cached_phc_time = 0x2555,
		.in_timestamp	 = 0x2,
		.expected	 = 0x2000,
	},
	{
		.desc		 = "gran = 12: forward small delta",
		.gran		 = 12,
		.cached_phc_time = 0x12345000,
		.in_timestamp	 = 0x12346,
		.expected	 = 0x12346000,
	},
	{
		.desc		 = "gran = 12: reverse small delta",
		.gran		 = 12,
		.cached_phc_time = 0x12346010,
		.in_timestamp	 = 0x12346,
		.expected	 = 0x12346000,
	},
	{
		.desc		 = "gran = 12: forward wrap across 35-bit ring",
		.gran		 = 12,
		.cached_phc_time = 0x00000017fffff000,
		.in_timestamp	 = 0x1,
		.expected	 = 0x0000001800001000,
	},
	{
		.desc		 = "gran = 12: reverse wrap across 35-bit ring",
		.gran		 = 12,
		.cached_phc_time = 0x0000001800001000,
		.in_timestamp	 = 0x7fffff,
		.expected	 = 0x00000017fffff000,
	},
	{
		.desc		 = "gran = 12: delta == half-range boundary",
		.gran		 = 12,
		.cached_phc_time = 0x0000000500001000,
		.in_timestamp	 = 0x100001,
		.expected	 = 0x0000000100001000,
	},
};

/*
 * Negative tests document expected aliasing when the cached PHC is too old:
 * if |delta| exceeds half of the modular range, extension cannot be unique.
 */
static const struct pkb_tstamp_negative_case pkb_tstamp_negative_cases[] = {
	/* gran = 7: |delta| = half_range + quantum (closest ambiguous cases). */
	{
		.desc		  = "gran = 7: stale cache near-boundary far-forward (half + q)",
		.gran		  = 7,
		.cached_phc_time  = 0x1234000000,
		.current_phc_time = 0x1254000080,
		.expected	  = 0x1214000080,
	},
	{
		.desc		  = "gran = 7: stale cache near-boundary far-backward (half + q)",
		.gran		  = 7,
		.cached_phc_time  = 0x1234000000,
		.current_phc_time = 0x1213ffff80,
		.expected	  = 0x1253ffff80,
	},
	/* gran = 7: |delta| is well beyond half_range. */
	{
		.desc		  = "gran = 7: stale cache aliases far-forward time",
		.gran		  = 7,
		.cached_phc_time  = 0x123fffff81,
		.current_phc_time = 0x126011ff89,
		.expected	  = 0x122011ff80,
	},
	{
		.desc		  = "gran = 7: stale cache aliases far-backward time",
		.gran		  = 7,
		.cached_phc_time  = 0x123fffff81,
		.current_phc_time = 0x121fffff01,
		.expected	  = 0x125fffff00,
	},
	/* gran = 9: |delta| = half_range + quantum (closest ambiguous cases). */
	{
		.desc		  = "gran = 9: stale cache near-boundary far-forward (half + q)",
		.gran		  = 9,
		.cached_phc_time  = 0x1234000000,
		.current_phc_time = 0x12b4000200,
		.expected	  = 0x11b4000200,
	},
	{
		.desc		  = "gran = 9: stale cache near-boundary far-backward (half + q)",
		.gran		  = 9,
		.cached_phc_time  = 0x1234000000,
		.current_phc_time = 0x11b3fffe00,
		.expected	  = 0x12b3fffe00,
	},
	/* gran = 9: |delta| is well beyond half_range. */
	{
		.desc		  = "gran = 9: stale cache aliases far-forward time",
		.gran		  = 9,
		.cached_phc_time  = 0x0000000100000000,
		.current_phc_time = 0x0000000180000200,
		.expected	  = 0x0000000080000200,
	},
	{
		.desc		  = "gran = 9: stale cache aliases far-backward time",
		.gran		  = 9,
		.cached_phc_time  = 0x0000000100000000,
		.current_phc_time = 0x000000007ffffe00,
		.expected	  = 0x000000017ffffe00,
	},
	/* gran = 12: |delta| = half_range + quantum (closest ambiguous cases). */
	{
		.desc		  = "gran = 12: stale cache near-boundary far-forward (half + q)",
		.gran		  = 12,
		.cached_phc_time  = 0x1234000000,
		.current_phc_time = 0x1634001000,
		.expected	  = 0x0e34001000,
	},
	{
		.desc		  = "gran = 12: stale cache near-boundary far-backward (half + q)",
		.gran		  = 12,
		.cached_phc_time  = 0x1234000000,
		.current_phc_time = 0x0e33fff000,
		.expected	  = 0x1633fff000,
	},
	/* gran = 12: |delta| is well beyond half_range. */
	{
		.desc		  = "gran = 12: stale cache aliases far-forward time",
		.gran		  = 12,
		.cached_phc_time  = 0x0000000500001000,
		.current_phc_time = 0x0000000900002000,
		.expected	  = 0x0000000100002000,
	},
	{
		.desc		  = "gran = 12: stale cache aliases far-backward time",
		.gran		  = 12,
		.cached_phc_time  = 0x0000000500001000,
		.current_phc_time = 0x0000000100000000,
		.expected	  = 0x0000000900000000,
	},
};

KUNIT_ARRAY_PARAM_DESC(pkb_tstamp, pkb_tstamp_cases, desc)

static void test_pkb_tstamp_extend_23b_to_64b(struct kunit *test)
{
	const struct pkb_tstamp_test_case *tc = test->param_value;
	u64 result;

	result = idpf_pkb_tstamp_extend_23b_to_64b(tc->cached_phc_time,
						   tc->in_timestamp,
						   tc->gran);
	KUNIT_EXPECT_EQ(test, result, tc->expected);
}

KUNIT_ARRAY_PARAM_DESC(pkb_tstamp_negative, pkb_tstamp_negative_cases, desc)

static void test_pkb_tstamp_extend_23b_to_64b_negative(struct kunit *test)
{
	const struct pkb_tstamp_negative_case *tc = test->param_value;
	u64 true_time, result;
	u32 in_timestamp;

	in_timestamp = (tc->current_phc_time >> tc->gran) & GENMASK(22, 0);
	true_time = tc->current_phc_time & ~(BIT_ULL(tc->gran) - 1);

	result = idpf_pkb_tstamp_extend_23b_to_64b(tc->cached_phc_time,
						   in_timestamp,
						   tc->gran);

	/*
	 * Result is expected to differ from the true quantized time when stale
	 * cache makes the extension ambiguous (> half modular range).
	 */
	KUNIT_EXPECT_NE(test, result, true_time);
	KUNIT_EXPECT_EQ(test, result, tc->expected);
}

#endif /* CONFIG_PTP_1588_CLOCK */

static struct kunit_case idpf_kunit_test_cases[] = {
#if IS_ENABLED(CONFIG_PTP_1588_CLOCK)
	KUNIT_CASE_PARAM(test_pkb_tstamp_extend_23b_to_64b,
			 pkb_tstamp_gen_params),
	KUNIT_CASE_PARAM(test_pkb_tstamp_extend_23b_to_64b_negative,
			 pkb_tstamp_negative_gen_params),
#endif /* CONFIG_PTP_1588_CLOCK */
	{}
};

static struct kunit_suite idpf_kunit_test_suite = {
	.name = "idpf-kunit",
	.test_cases = idpf_kunit_test_cases,
};

kunit_test_suite(idpf_kunit_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_DESCRIPTION("KUnit tests for IDPF driver");
MODULE_LICENSE("GPL");
