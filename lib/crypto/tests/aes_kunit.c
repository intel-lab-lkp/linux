// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include "aes-testvecs.h"

static void test_aes(struct kunit *test, const struct aes_testvector *tv,
		     bool enc)
{
	struct aes_key aes_key;
	u8 out[AES_BLOCK_SIZE];
	const u8 *input, *expect;
	int rc;

	rc = aes_preparekey(&aes_key, tv->key.b, tv->key.len);
	KUNIT_ASSERT_EQ(test, 0, rc);

	if (enc) {
		input = tv->plain;
		expect = tv->cipher;
		aes_encrypt(&aes_key, out, input);
	} else {
		input = tv->cipher;
		expect = tv->plain;
		aes_decrypt(&aes_key, out, input);
	}
	KUNIT_ASSERT_MEMEQ(test, out, expect, sizeof(out));
}

static void benchmark_aes(struct kunit *test, const struct aes_testvector *tv)
{
	const size_t num_iters = 10000000;
	u8 out[AES_BLOCK_SIZE];
	struct aes_key aes_key;
	u64 t_enc, t_dec;
	int rc;

	if (!IS_ENABLED(CONFIG_CRYPTO_LIB_BENCHMARK))
		kunit_skip(test, "not enabled");

	rc = aes_preparekey(&aes_key, tv->key.b, tv->key.len);
	KUNIT_ASSERT_EQ(test, 0, rc);

	/* warm-up enc */
	for (size_t i = 0; i < 1000; i++)
		aes_encrypt(&aes_key, out, tv->plain);

	preempt_disable();
	t_enc = ktime_get_ns();

	for (size_t i = 0; i < num_iters; i++)
		aes_encrypt(&aes_key, out, tv->plain);

	t_enc = ktime_get_ns() - t_enc;
	preempt_enable();

	/* warm-up dec */
	for (size_t i = 0; i < 1000; i++)
		aes_decrypt(&aes_key, out, tv->cipher);

	preempt_disable();
	t_dec = ktime_get_ns();

	for (size_t i = 0; i < num_iters; i++)
		aes_decrypt(&aes_key, out, tv->cipher);

	t_dec = ktime_get_ns() - t_dec;
	preempt_enable();

	kunit_info(test, "enc (iter. %zu, duration %lluns)",
		   num_iters, t_enc);
	kunit_info(test, "enc (len=%zu): %llu MB/s",
		   (size_t)AES_BLOCK_SIZE,
		   div64_u64((u64)AES_BLOCK_SIZE * num_iters * NSEC_PER_SEC,
			     (t_enc ?: 1) * SZ_1M));

	kunit_info(test, "dec (iter. %zu, duration %lluns)",
		   num_iters, t_dec);
	kunit_info(test, "dec (len=%zu): %llu MB/s",
		   (size_t)AES_BLOCK_SIZE,
		   div64_u64((u64)AES_BLOCK_SIZE * num_iters * NSEC_PER_SEC,
			     (t_dec ?: 1) * SZ_1M));
}

static void test_aes128_encrypt(struct kunit *test)
{
	test_aes(test, &aes128_kat, true);
}

static void test_aes128_decrypt(struct kunit *test)
{
	test_aes(test, &aes128_kat, false);
}

static void test_aes192_encrypt(struct kunit *test)
{
	test_aes(test, &aes192_kat, true);
}

static void test_aes192_decrypt(struct kunit *test)
{
	test_aes(test, &aes192_kat, false);
}

static void test_aes256_encrypt(struct kunit *test)
{
	test_aes(test, &aes256_kat, true);
}

static void test_aes256_decrypt(struct kunit *test)
{
	test_aes(test, &aes256_kat, false);
}

static void benchmark_aes128(struct kunit *test)
{
	benchmark_aes(test, &aes128_kat);
}

static void benchmark_aes192(struct kunit *test)
{
	benchmark_aes(test, &aes192_kat);
}

static void benchmark_aes256(struct kunit *test)
{
	benchmark_aes(test, &aes256_kat);
}

static struct kunit_case aes_test_cases[] = {
	KUNIT_CASE(test_aes128_encrypt),
	KUNIT_CASE(test_aes128_decrypt),
	KUNIT_CASE(test_aes192_encrypt),
	KUNIT_CASE(test_aes192_decrypt),
	KUNIT_CASE(test_aes256_encrypt),
	KUNIT_CASE(test_aes256_decrypt),
	KUNIT_CASE(benchmark_aes128),
	KUNIT_CASE(benchmark_aes192),
	KUNIT_CASE(benchmark_aes256),
	{},
};

static struct kunit_suite aes_test_suite = {
	.name = "aes",
	.test_cases = aes_test_cases,
};

kunit_test_suite(aes_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark aes library");
MODULE_LICENSE("GPL");
