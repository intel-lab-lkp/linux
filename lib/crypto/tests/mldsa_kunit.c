// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Copyright (C) 2025 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <crypto/mldsa.h>
#include <kunit/test.h>

struct mldsa_testvector {
	u16 pk_len;
	u16 msg_len;
	u16 sig_len;
	const char *what;
	const char *algo;
	const u8 *pk;
	const u8 *sig;
	const u8 *msg;
};

/*
 * Use rejection test vector which will cover all rejection code paths
 * as generated with the mldsa_edge_case_tester.
 */
static const struct mldsa_testvector mldsa_44_testvectors[] = {
#include "mldsa_pure_rejection_vectors_44.h"
};
static const struct mldsa_testvector mldsa_65_testvectors[] = {
#include "mldsa_pure_rejection_vectors_65.h"
};
static const struct mldsa_testvector mldsa_87_testvectors[] = {
#include "mldsa_pure_rejection_vectors_87.h"
};

static void do_mldsa_and_assert_success(struct kunit *test, enum mldsa_alg alg,
					const struct mldsa_testvector *tv)
{
	int err = mldsa_verify(alg, tv->sig, tv->sig_len, tv->msg, tv->msg_len,
			       tv->pk, tv->pk_len);
	KUNIT_ASSERT_EQ(test, err, 0);
}

static void test_mldsa(struct kunit *test, enum mldsa_alg alg,
		       const struct mldsa_testvector *tvs, size_t num_tvs)
{
	for (size_t i = 0; i < num_tvs; i++)
		do_mldsa_and_assert_success(test, alg, &tvs[i]);
}

static void test_mldsa44(struct kunit *test)
{
	test_mldsa(test, MLDSA44, mldsa_44_testvectors,
		   ARRAY_SIZE(mldsa_44_testvectors));
}

static void test_mldsa65(struct kunit *test)
{
	test_mldsa(test, MLDSA65, mldsa_65_testvectors,
		   ARRAY_SIZE(mldsa_65_testvectors));
}

static void test_mldsa87(struct kunit *test)
{
	test_mldsa(test, MLDSA87, mldsa_87_testvectors,
		   ARRAY_SIZE(mldsa_87_testvectors));
}

static void benchmark_mldsa(struct kunit *test, enum mldsa_alg alg,
			    const struct mldsa_testvector *tv)
{
	const int warmup_niter = 200;
	const int benchmark_niter = 200;
	u64 t0, t1;

	if (!IS_ENABLED(CONFIG_CRYPTO_LIB_BENCHMARK))
		kunit_skip(test, "not enabled");

	/* Warm-up */
	for (int i = 0; i < warmup_niter; i++)
		do_mldsa_and_assert_success(test, alg, tv);

	t0 = ktime_get_ns();
	for (int i = 0; i < benchmark_niter; i++)
		do_mldsa_and_assert_success(test, alg, tv);
	t1 = ktime_get_ns();
	kunit_info(test, "%llu ops/s",
		   div64_u64((u64)benchmark_niter * NSEC_PER_SEC,
			     t1 - t0 ?: 1));
}

static void benchmark_mldsa44(struct kunit *test)
{
	benchmark_mldsa(test, MLDSA44, &mldsa_44_testvectors[0]);
}

static void benchmark_mldsa65(struct kunit *test)
{
	benchmark_mldsa(test, MLDSA65, &mldsa_65_testvectors[0]);
}

static void benchmark_mldsa87(struct kunit *test)
{
	benchmark_mldsa(test, MLDSA87, &mldsa_87_testvectors[0]);
}

static struct kunit_case mldsa_kunit_cases[] = {
	KUNIT_CASE(test_mldsa44),
	KUNIT_CASE(test_mldsa65),
	KUNIT_CASE(test_mldsa87),
	KUNIT_CASE(benchmark_mldsa44),
	KUNIT_CASE(benchmark_mldsa65),
	KUNIT_CASE(benchmark_mldsa87),
	{},
};

static struct kunit_suite mldsa_kunit_suite = {
	.name = "mldsa",
	.test_cases = mldsa_kunit_cases,
};
kunit_test_suite(mldsa_kunit_suite);

MODULE_AUTHOR("David Howells <dhowells@redhat.com>");
MODULE_DESCRIPTION("ML-DSA tests");
MODULE_LICENSE("Dual BSD/GPL");
