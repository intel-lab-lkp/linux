// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>

#include "aes-testvecs.h"

#define AES_KAT(bits, func, from, to)				\
static void aes##bits##_kat_##func(struct kunit *test)		\
{								\
	const u8 *in = AES##bits##_KAT.from;			\
	u8 out[AES_BLOCK_SIZE];					\
	struct aes_key aes_key;					\
								\
	if (aes_preparekey(&aes_key, AES##bits##_KAT.key.b,	\
			   AES##bits##_KAT.key.len))		\
		kunit_skip(test, "no key");			\
								\
	aes_##func(&aes_key, out, in);				\
	KUNIT_ASSERT_MEMEQ(test, out, AES##bits##_KAT.to,	\
			   sizeof(out));			\
}

#define KB		(1024)
#define MB		(KB * KB)
#define NS_PER_SEC	(1000000000ULL)

#define AES_BENCHMARK(bits)					\
static void aes##bits##_benchmark(struct kunit *test)		\
{								\
	const size_t num_iters = 10000000;			\
	const u8 *cipher = AES##bits##_KAT.cipher;		\
	const u8 *plain = AES##bits##_KAT.plain;		\
	u8 out[AES_BLOCK_SIZE];					\
	struct aes_key aes_key;					\
	u64 t_enc, t_dec;					\
								\
	if (!IS_ENABLED(CONFIG_CRYPTO_LIB_BENCHMARK))		\
		kunit_skip(test, "not enabled");		\
								\
	if (aes_preparekey(&aes_key, AES##bits##_KAT.key.b,	\
			   AES##bits##_KAT.key.len))		\
		kunit_skip(test, "no key");			\
								\
	/* warm-up enc */					\
	for (size_t i = 0; i < 1000; i++)			\
		aes_encrypt(&aes_key, out, plain);		\
								\
	preempt_disable();					\
	t_enc = ktime_get_ns();					\
								\
	for (size_t i = 0; i < num_iters; i++)			\
		aes_encrypt(&aes_key, out, plain);		\
								\
	t_enc = ktime_get_ns() - t_enc;				\
	preempt_enable();					\
								\
	/* warm-up dec */					\
	for (size_t i = 0; i < 1000; i++)			\
		aes_decrypt(&aes_key, out, cipher);		\
								\
	preempt_disable();					\
	t_dec = ktime_get_ns();					\
								\
	for (size_t i = 0; i < num_iters; i++)			\
		aes_decrypt(&aes_key, out, cipher);		\
								\
	t_dec = ktime_get_ns() - t_dec;				\
	preempt_enable();					\
								\
	kunit_info(test, "enc (iter. %zu, duration %lluns)",	\
		   num_iters, t_enc);				\
	kunit_info(test, "enc (len=%zu): %llu MB/s",		\
		   (size_t)AES_BLOCK_SIZE,			\
		   div64_u64((u64)AES_BLOCK_SIZE * num_iters * NS_PER_SEC, \
			     (t_enc ?: 1) * MB));		\
								\
	kunit_info(test, "dec (iter. %zu, duration %lluns)",	\
		   num_iters, t_dec);				\
	kunit_info(test, "dec (len=%zu): %llu MB/s",		\
		   (size_t)AES_BLOCK_SIZE,			\
		   div64_u64((u64)AES_BLOCK_SIZE * num_iters * NS_PER_SEC, \
			     (t_dec ?: 1) * MB));		\
}

AES_KAT(128, encrypt, plain, cipher);
AES_KAT(192, encrypt, plain, cipher);
AES_KAT(256, encrypt, plain, cipher);
AES_KAT(128, decrypt, cipher, plain);
AES_KAT(192, decrypt, cipher, plain);
AES_KAT(256, decrypt, cipher, plain);
AES_BENCHMARK(128);
AES_BENCHMARK(192);
AES_BENCHMARK(256);

static struct kunit_case aes_test_cases[] = {
	KUNIT_CASE(aes128_kat_encrypt),
	KUNIT_CASE(aes128_kat_decrypt),
	KUNIT_CASE(aes192_kat_encrypt),
	KUNIT_CASE(aes192_kat_decrypt),
	KUNIT_CASE(aes256_kat_encrypt),
	KUNIT_CASE(aes256_kat_decrypt),
	KUNIT_CASE(aes128_benchmark),
	KUNIT_CASE(aes192_benchmark),
	KUNIT_CASE(aes256_benchmark),
	{},
};

static struct kunit_suite aes_test_suite = {
	.name = "aes",
	.test_cases = aes_test_cases,
};

kunit_test_suite(aes_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark aes library");
MODULE_LICENSE("GPL");
