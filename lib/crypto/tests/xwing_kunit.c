// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests for X-Wing
 *
 * This should be run together with the mlkem and curve25519 KUnit tests.
 *
 * Copyright 2026 Google LLC
 */
#include <crypto/sha3.h>
#include <crypto/xwing.h>
#include <kunit/test.h>

#include "xwing-testvecs.h"

struct xwing_bufs {
	u8 pk[XWING_PUBLIC_KEY_BYTES];
	u8 sk[XWING_SECRET_KEY_BYTES];
	u8 ct[XWING_CIPHERTEXT_BYTES];
	u8 ss[XWING_SHARED_SECRET_BYTES];
	u8 pk_hash[SHA3_256_DIGEST_SIZE];
	u8 sk_hash[SHA3_256_DIGEST_SIZE];
	u8 ct_hash[SHA3_256_DIGEST_SIZE];
};

static struct xwing_bufs *alloc_bufs(struct kunit *test)
{
	struct xwing_bufs *bufs =
		kunit_kmalloc(test, sizeof(*bufs), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bufs);
	return bufs;
}

static void test_xwing_rfc_testvecs(struct kunit *test)
{
	struct xwing_bufs *bufs = alloc_bufs(test);

	for (size_t i = 0; i < ARRAY_SIZE(xwing_testvecs); i++) {
		const struct xwing_testvec *tv = &xwing_testvecs[i];

		KUNIT_ASSERT_EQ(test, 0,
				xwing_keygen_internal(bufs->pk, bufs->sk,
						      tv->seed));
		sha3_256(bufs->pk, sizeof(bufs->pk), bufs->pk_hash);
		sha3_256(bufs->sk, sizeof(bufs->sk), bufs->sk_hash);
		KUNIT_ASSERT_MEMEQ(test, tv->pk_hash, bufs->pk_hash,
				   sizeof(tv->pk_hash));
		KUNIT_ASSERT_MEMEQ(test, tv->sk_hash, bufs->sk_hash,
				   sizeof(tv->sk_hash));

		KUNIT_ASSERT_EQ(test, 0,
				xwing_encaps_internal(bufs->ct, bufs->ss,
						      bufs->pk, tv->eseed));
		sha3_256(bufs->ct, sizeof(bufs->ct), bufs->ct_hash);
		KUNIT_ASSERT_MEMEQ(test, tv->ct_hash, bufs->ct_hash,
				   sizeof(tv->ct_hash));
		KUNIT_ASSERT_MEMEQ(test, tv->ss, bufs->ss, sizeof(bufs->ss));

		memset(bufs->ss, 0xff, sizeof(bufs->ss));
		KUNIT_ASSERT_EQ(test, 0,
				xwing_decaps(bufs->ss, bufs->ct, bufs->sk));
		KUNIT_ASSERT_MEMEQ(test, tv->ss, bufs->ss, sizeof(bufs->ss));
	}
}

static void test_xwing_round_trip(struct kunit *test)
{
	struct xwing_bufs *bufs = alloc_bufs(test);
	u8 ss2[XWING_SHARED_SECRET_BYTES];

	for (int i = 0; i < 20; i++) {
		KUNIT_ASSERT_EQ(test, 0, xwing_keygen(bufs->pk, bufs->sk));
		KUNIT_ASSERT_EQ(test, 0,
				xwing_encaps(bufs->ct, bufs->ss, bufs->pk));
		KUNIT_ASSERT_EQ(test, 0, xwing_decaps(ss2, bufs->ct, bufs->sk));
		KUNIT_ASSERT_MEMEQ(test, bufs->ss, ss2, sizeof(bufs->ss));
	}
}

/* Benchmark X-Wing performance. */
static void benchmark_xwing(struct kunit *test)
{
	struct xwing_bufs *bufs = alloc_bufs(test);
	const int iterations = 100;
	ktime_t start, end;

	if (!IS_ENABLED(CONFIG_CRYPTO_LIB_BENCHMARK))
		kunit_skip(test, "not enabled");

	start = ktime_get();
	for (int i = 0; i < iterations; i++)
		KUNIT_ASSERT_EQ(test, 0, xwing_keygen(bufs->pk, bufs->sk));
	end = ktime_get();
	kunit_info(test, "XWing_KeyGen: %llu ns/op\n",
		   div64_u64(ktime_to_ns(ktime_sub(end, start)), iterations));

	start = ktime_get();
	for (int i = 0; i < iterations; i++)
		KUNIT_ASSERT_EQ(test, 0,
				xwing_encaps(bufs->ct, bufs->ss, bufs->pk));
	end = ktime_get();
	kunit_info(test, "XWing_Encaps: %llu ns/op\n",
		   div64_u64(ktime_to_ns(ktime_sub(end, start)), iterations));

	start = ktime_get();
	for (int i = 0; i < iterations; i++)
		KUNIT_ASSERT_EQ(test, 0,
				xwing_decaps(bufs->ss, bufs->ct, bufs->sk));
	end = ktime_get();
	kunit_info(test, "XWing_Decaps: %llu ns/op\n",
		   div64_u64(ktime_to_ns(ktime_sub(end, start)), iterations));
}

static struct kunit_case xwing_test_cases[] = {
	KUNIT_CASE(test_xwing_rfc_testvecs),
	KUNIT_CASE(test_xwing_round_trip),
	KUNIT_CASE(benchmark_xwing),
	{}
};

static struct kunit_suite xwing_test_suite = {
	.name = "xwing",
	.test_cases = xwing_test_cases,
};
kunit_test_suite(xwing_test_suite);

MODULE_DESCRIPTION("KUnit tests for X-Wing");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
