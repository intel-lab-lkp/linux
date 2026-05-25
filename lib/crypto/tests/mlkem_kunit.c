// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests and benchmark for ML-KEM
 *
 * Copyright 2026 Google LLC
 */
#include <crypto/mlkem.h>
#include <crypto/sha3.h>
#include <kunit/test.h>
#include "mlkem-testvecs.h"

#define Q 3329

enum mlkem_paramset {
	MLKEM768,
	MLKEM1024,
};

struct mlkem_bufs {
	u8 *pk, *sk, *ct;
	size_t pk_len, sk_len, ct_len;
	u8 ss[MLKEM_SHARED_SECRET_BYTES];
	u8 seed[MLKEM_SEED_BYTES];
	u8 eseed[MLKEM_ESEED_BYTES];
};

static const struct {
	const char *name;
	int k;
	size_t pk_len;
	size_t sk_len;
	size_t ct_len;
} mlkem_paramsets[] = {
	[MLKEM768] = {
		.name = "ML-KEM-768",
		.k = 3,
		.pk_len = MLKEM768_PUBLIC_KEY_BYTES,
		.sk_len = MLKEM768_SECRET_KEY_BYTES,
		.ct_len = MLKEM768_CIPHERTEXT_BYTES,
	},
	[MLKEM1024] = {
		.name = "ML-KEM-1024",
		.k = 4,
		.pk_len = MLKEM1024_PUBLIC_KEY_BYTES,
		.sk_len = MLKEM1024_SECRET_KEY_BYTES,
		.ct_len = MLKEM1024_CIPHERTEXT_BYTES,
	},
};

static struct mlkem_bufs *alloc_bufs(struct kunit *test,
				     enum mlkem_paramset paramset)
{
	struct mlkem_bufs *bufs =
		kunit_kmalloc(test, sizeof(*bufs), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, bufs);

	bufs->pk_len = mlkem_paramsets[paramset].pk_len;
	bufs->pk = kunit_kmalloc(test, bufs->pk_len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bufs->pk);

	bufs->sk_len = mlkem_paramsets[paramset].sk_len;
	bufs->sk = kunit_kmalloc(test, bufs->sk_len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bufs->sk);

	bufs->ct_len = mlkem_paramsets[paramset].ct_len;
	bufs->ct = kunit_kmalloc(test, bufs->ct_len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, bufs->ct);

	return bufs;
}

static int keygen(u8 *pk, u8 *sk, enum mlkem_paramset paramset)
{
	switch (paramset) {
	case MLKEM768:
		return mlkem768_keygen(pk, sk);
	case MLKEM1024:
		return mlkem1024_keygen(pk, sk);
	default:
		WARN_ON_ONCE(1);
		return -EOPNOTSUPP;
	}
}

static int keygen_internal(u8 *pk, u8 *sk, const u8 seed[MLKEM_SEED_BYTES],
			   enum mlkem_paramset paramset)
{
	switch (paramset) {
	case MLKEM768:
		return mlkem768_keygen_internal(pk, sk, seed);
	case MLKEM1024:
		return mlkem1024_keygen_internal(pk, sk, seed);
	default:
		WARN_ON_ONCE(1);
		return -EOPNOTSUPP;
	}
}

static int encaps(u8 *ct, u8 ss[MLKEM_SHARED_SECRET_BYTES], const u8 *pk,
		  enum mlkem_paramset paramset)
{
	switch (paramset) {
	case MLKEM768:
		return mlkem768_encaps(ct, ss, pk);
	case MLKEM1024:
		return mlkem1024_encaps(ct, ss, pk);
	default:
		WARN_ON_ONCE(1);
		return -EOPNOTSUPP;
	}
}

static int encaps_internal(u8 *ct, u8 ss[MLKEM_SHARED_SECRET_BYTES],
			   const u8 *pk, const u8 eseed[MLKEM_ESEED_BYTES],
			   enum mlkem_paramset paramset)
{
	switch (paramset) {
	case MLKEM768:
		return mlkem768_encaps_internal(ct, ss, pk, eseed);
	case MLKEM1024:
		return mlkem1024_encaps_internal(ct, ss, pk, eseed);
	default:
		WARN_ON_ONCE(1);
		return -EOPNOTSUPP;
	}
}

static int decaps(u8 ss[MLKEM_SHARED_SECRET_BYTES], const u8 *ct, const u8 *sk,
		  enum mlkem_paramset paramset)
{
	switch (paramset) {
	case MLKEM768:
		return mlkem768_decaps(ss, ct, sk);
	case MLKEM1024:
		return mlkem1024_decaps(ss, ct, sk);
	default:
		WARN_ON_ONCE(1);
		return -EOPNOTSUPP;
	}
}

/*
 * Test the ML-KEM implementation against the first 1000 test vectors from the
 * reference implementation.
 *
 * To do this without explicitly including all these test vectors, which would
 * result in a massive source and binary size, we take advantage of the fact
 * that the reference test vectors are generated deterministically (by
 * kyber/ref/tests/test_vectors.c).  We just regenerate them at runtime using
 * the same algorithm.  We hash all the outputs, then verify that hash against
 * @expected_cumulative_hash, which proves that all the outputs were correct.
 */
static void
test_mlkem_against_ref_testvecs(struct kunit *test, size_t num_testvecs,
				const u8 expected_cumulative_hash[32],
				enum mlkem_paramset paramset)
{
	struct mlkem_bufs *bufs = alloc_bufs(test, paramset);
	struct shake_ctx cumulative_hash_ctx;
	struct shake_ctx seed_ctx;
	u8 cumulative_hash[32];

	shake128_init(&cumulative_hash_ctx);
	shake128_init(&seed_ctx);
	for (size_t i = 0; i < num_testvecs; i++) {
		/*
		 * Deterministically generate the next seeds using the same
		 * algorithm as the reference code's test_vectors.c.
		 */
		shake_squeeze(&seed_ctx, bufs->seed, sizeof(bufs->seed));
		shake_squeeze(&seed_ctx, bufs->eseed, sizeof(bufs->eseed));

		/* KeyGen, then update with (pk, sk) */
		KUNIT_ASSERT_EQ(test, 0,
				keygen_internal(bufs->pk, bufs->sk, bufs->seed,
						paramset));
		shake_update(&cumulative_hash_ctx, bufs->pk, bufs->pk_len);
		shake_update(&cumulative_hash_ctx, bufs->sk, bufs->sk_len);

		/* Encaps, then update with (ct, ss) */
		KUNIT_ASSERT_EQ(test, 0,
				encaps_internal(bufs->ct, bufs->ss, bufs->pk,
						bufs->eseed, paramset));
		shake_update(&cumulative_hash_ctx, bufs->ct, bufs->ct_len);
		shake_update(&cumulative_hash_ctx, bufs->ss, sizeof(bufs->ss));

		/* Decaps, then update with ss */
		memset(bufs->ss, 0xff, sizeof(bufs->ss));
		KUNIT_ASSERT_EQ(test, 0,
				decaps(bufs->ss, bufs->ct, bufs->sk, paramset));
		shake_update(&cumulative_hash_ctx, bufs->ss, sizeof(bufs->ss));

		/*
		 * Deterministically generate an invalid ciphertext, using the
		 * same algorithm as test_vectors.c.  Then do Decaps and update
		 * with ss_rejected.  This tests the implicit rejection case.
		 */
		shake_squeeze(&seed_ctx, bufs->ct, bufs->ct_len);
		KUNIT_ASSERT_EQ(test, 0,
				decaps(bufs->ss, bufs->ct, bufs->sk, paramset));
		shake_update(&cumulative_hash_ctx, bufs->ss, sizeof(bufs->ss));
	}
	/*
	 * Finalize and verify the cumulative hash.  This verifies that every
	 * (pk, sk, ct, ss, ss, ss_rejected) tuple was correct.
	 */
	shake_squeeze(&cumulative_hash_ctx, cumulative_hash,
		      sizeof(cumulative_hash));
	KUNIT_ASSERT_MEMEQ(test, expected_cumulative_hash, cumulative_hash,
			   sizeof(cumulative_hash));
}

static void test_mlkem_round_trip(struct kunit *test,
				  enum mlkem_paramset paramset)
{
	struct mlkem_bufs *bufs = alloc_bufs(test, paramset);
	u8 ss2[MLKEM_SHARED_SECRET_BYTES];

	for (int i = 0; i < 20; i++) {
		KUNIT_ASSERT_EQ(test, 0, keygen(bufs->pk, bufs->sk, paramset));
		KUNIT_ASSERT_EQ(test, 0,
				encaps(bufs->ct, bufs->ss, bufs->pk, paramset));
		KUNIT_ASSERT_EQ(test, 0,
				decaps(ss2, bufs->ct, bufs->sk, paramset));
		KUNIT_ASSERT_MEMEQ(test, bufs->ss, ss2, sizeof(bufs->ss));
	}
}

/*
 * Test that changing any part of the ciphertext results in a different shared
 * secret due to implicit rejection.
 */
static void test_mlkem_rejection(struct kunit *test,
				 enum mlkem_paramset paramset)
{
	struct mlkem_bufs *bufs = alloc_bufs(test, paramset);
	u8 ss2[MLKEM_SHARED_SECRET_BYTES];

	KUNIT_ASSERT_EQ(test, 0, keygen(bufs->pk, bufs->sk, paramset));
	KUNIT_ASSERT_EQ(test, 0,
			encaps(bufs->ct, bufs->ss, bufs->pk, paramset));

	/* Decapsulate a valid ciphertext. */
	KUNIT_ASSERT_EQ(test, 0, decaps(ss2, bufs->ct, bufs->sk, paramset));
	KUNIT_ASSERT_MEMEQ(test, bufs->ss, ss2, sizeof(bufs->ss));

	for (size_t i = 0; i < bufs->ct_len; i++) {
		/* Corrupt byte i of the ciphertext. */
		bufs->ct[i] ^= 1;

		/* Decapsulate an invalid ciphertext and assert ss differs. */
		KUNIT_ASSERT_EQ(test, 0,
				decaps(ss2, bufs->ct, bufs->sk, paramset));
		KUNIT_ASSERT_MEMNEQ(test, bufs->ss, ss2, sizeof(bufs->ss));
		/* Undo the ciphertext corruption. */
		bufs->ct[i] ^= 1;
	}
}

/*
 * Test that the encapsulation function returns -EBADMSG if a coefficient in
 * NTT(t) in the public key is outside the interval [0, Q - 1].
 */
static void test_mlkem_invalid_pk(struct kunit *test,
				  enum mlkem_paramset paramset)
{
	struct mlkem_bufs *bufs = alloc_bufs(test, paramset);
	const size_t ntt_t_len = 384 * mlkem_paramsets[paramset].k;

	for (int i = 0; i < 4; i++) {
		u16 c;

		KUNIT_ASSERT_EQ(test, 0, keygen(bufs->pk, bufs->sk, paramset));
		KUNIT_ASSERT_EQ(test, 0,
				encaps(bufs->ct, bufs->ss, bufs->pk, paramset));
		/*
		 * Corrupt a coefficient of NTT(t), which is an array of 256*k
		 * 12-bit coefficients starting at the beginning of pk.
		 */
		if (i % 2 == 0)
			c = Q; /* Low end of invalid range */
		else
			c = 0xfff; /* High end of invalid range */
		if (i < 2) {
			/* Corrupt the first 12-bit coefficient in NTT(t). */
			bufs->pk[0] = (c & 0xff);
			bufs->pk[1] = (bufs->pk[1] & 0xf0) | (c >> 8);
		} else {
			/* Corrupt the last 12-bit coefficient in NTT(t). */
			bufs->pk[ntt_t_len - 2] =
				(bufs->pk[ntt_t_len - 2] & 0xf) |
				((c & 0xf) << 4);
			bufs->pk[ntt_t_len - 1] = c >> 4;
		}
		KUNIT_ASSERT_EQ(test, -EBADMSG,
				encaps(bufs->ct, bufs->ss, bufs->pk, paramset));
	}
}

/*
 * Test that the decapsulation function returns -EBADMSG if either:
 *
 *    - H(pk) is corrupt
 *    - A coefficient in NTT(s) is outside the interval [0, Q - 1]
 */
static void test_mlkem_invalid_sk(struct kunit *test,
				  enum mlkem_paramset paramset)
{
	struct mlkem_bufs *bufs = alloc_bufs(test, paramset);
	const size_t ntt_s_len = 384 * mlkem_paramsets[paramset].k;

	KUNIT_ASSERT_EQ(test, 0, keygen(bufs->pk, bufs->sk, paramset));
	KUNIT_ASSERT_EQ(test, 0,
			encaps(bufs->ct, bufs->ss, bufs->pk, paramset));
	KUNIT_ASSERT_EQ(test, 0,
			decaps(bufs->ss, bufs->ct, bufs->sk, paramset));

	/* Corrupt H(pk) in the sk. */
	bufs->sk[bufs->sk_len - 33] ^= 0x80;
	KUNIT_ASSERT_EQ(test, -EBADMSG,
			decaps(bufs->ss, bufs->ct, bufs->sk, paramset));

	for (int i = 0; i < 4; i++) {
		u16 c;

		KUNIT_ASSERT_EQ(test, 0, keygen(bufs->pk, bufs->sk, paramset));
		KUNIT_ASSERT_EQ(test, 0,
				encaps(bufs->ct, bufs->ss, bufs->pk, paramset));
		KUNIT_ASSERT_EQ(test, 0,
				decaps(bufs->ss, bufs->ct, bufs->sk, paramset));

		/*
		 * Corrupt a coefficient of NTT(s), which is an array of 256*k
		 * 12-bit coefficients starting at the beginning of sk.
		 */
		if (i % 2 == 0)
			c = Q; /* Low end of invalid range */
		else
			c = 0xfff; /* High end of invalid range */
		if (i < 2) {
			/* Corrupt the first 12-bit coefficient in NTT(s). */
			bufs->sk[0] = (c & 0xff);
			bufs->sk[1] = (bufs->sk[1] & 0xf0) | (c >> 8);
		} else {
			/* Corrupt the last 12-bit coefficient in NTT(s). */
			bufs->sk[ntt_s_len - 2] =
				(bufs->sk[ntt_s_len - 2] & 0xf) |
				((c & 0xf) << 4);
			bufs->sk[ntt_s_len - 1] = c >> 4;
		}
		KUNIT_ASSERT_EQ(test, -EBADMSG,
				decaps(bufs->ss, bufs->ct, bufs->sk, paramset));
	}
}

static void test_mlkem(struct kunit *test, size_t num_testvecs,
		       const u8 expected_cumulative_hash[32],
		       enum mlkem_paramset paramset)
{
	test_mlkem_against_ref_testvecs(test, num_testvecs,
					expected_cumulative_hash, paramset);
	test_mlkem_round_trip(test, paramset);
	test_mlkem_rejection(test, paramset);
	test_mlkem_invalid_pk(test, paramset);
	test_mlkem_invalid_sk(test, paramset);
}

static void test_mlkem768(struct kunit *test)
{
	test_mlkem(test, MLKEM768_NUM_TESTVECS, mlkem768_hash, MLKEM768);
}

static void test_mlkem1024(struct kunit *test)
{
	test_mlkem(test, MLKEM1024_NUM_TESTVECS, mlkem1024_hash, MLKEM1024);
}

static u16 mod_q(s32 x)
{
	x %= Q;
	if (x < 0)
		x += Q;
	return x;
}

/*
 * Test that mlkem_reduce_once() and mlkem_reduce() produce the correct output
 * for every supported input.
 */
static void test_mlkem_reduce(struct kunit *test)
{
	/* mlkem_reduce_once() supports 0 <= x < 2*Q */
	for (u16 x = 0; x < 2 * Q; x++)
		KUNIT_ASSERT_EQ(test, mod_q(x), mlkem_reduce_once(x));

	/* mlkem_reduce() supports 0 <= x < Q + 2*Q*Q */
	for (u32 x = 0; x < Q + 2 * Q * Q; x++)
		KUNIT_ASSERT_EQ(test, mod_q(x), mlkem_reduce(x));
}

/* round((2^d / Q) * x) mod 2^d */
static u16 compress_d_ref(u16 x, int d)
{
	u64 quotient, remainder;

	quotient = div64_u64_rem((u64)x << d, Q, &remainder);
	if (remainder >= (Q + 1) / 2)
		quotient++;
	return quotient & ((1 << d) - 1);
}

/* round((Q / 2^d) * y) */
static u16 decompress_d_ref(u16 y, int d)
{
	u64 quotient, remainder;

	quotient = div64_u64_rem((u64)y * Q, 1 << d, &remainder);
	if (remainder >= 1 << (d - 1))
		quotient++;
	return quotient;
}

/*
 * Test that mlkem_compress_d() produces the correct output for every supported
 * input.
 */
static void test_mlkem_compress(struct kunit *test)
{
	/* compress_d() supports 0 <= x < Q and 1 <= d <= 11. */
	for (int d = 1; d <= 11; d++) {
		for (int x = 0; x < Q; x++) {
			KUNIT_ASSERT_EQ(test, compress_d_ref(x, d),
					mlkem_compress_d(x, d));
		}
	}
}

/*
 * Test that mlkem_decompress_d() produces the correct output for every
 * supported input.
 */
static void test_mlkem_decompress(struct kunit *test)
{
	for (int d = 1; d <= 11; d++) {
		for (int y = 0; y < (1 << d); y++) {
			KUNIT_ASSERT_EQ(test, decompress_d_ref(y, d),
					mlkem_decompress_d(y, d));
		}
	}
}

/* Benchmark ML-KEM performance. */
static void benchmark_mlkem(struct kunit *test, enum mlkem_paramset paramset)
{
	const char *name = mlkem_paramsets[paramset].name;
	struct mlkem_bufs *bufs = alloc_bufs(test, paramset);
	const int iterations = 100;
	ktime_t start, end;

	if (!IS_ENABLED(CONFIG_CRYPTO_LIB_BENCHMARK))
		kunit_skip(test, "not enabled");

	start = ktime_get();
	for (int i = 0; i < iterations; i++)
		KUNIT_ASSERT_EQ(test, 0, keygen(bufs->pk, bufs->sk, paramset));
	end = ktime_get();
	kunit_info(test, "%s_KeyGen: %llu ns/op\n", name,
		   div64_u64(ktime_to_ns(ktime_sub(end, start)), iterations));

	start = ktime_get();
	for (int i = 0; i < iterations; i++)
		KUNIT_ASSERT_EQ(test, 0,
				encaps(bufs->ct, bufs->ss, bufs->pk, paramset));
	end = ktime_get();
	kunit_info(test, "%s_Encaps: %llu ns/op\n", name,
		   div64_u64(ktime_to_ns(ktime_sub(end, start)), iterations));

	start = ktime_get();
	for (int i = 0; i < iterations; i++)
		KUNIT_ASSERT_EQ(test, 0,
				decaps(bufs->ss, bufs->ct, bufs->sk, paramset));
	end = ktime_get();
	kunit_info(test, "%s_Decaps: %llu ns/op\n", name,
		   div64_u64(ktime_to_ns(ktime_sub(end, start)), iterations));
}

static void benchmark_mlkem768(struct kunit *test)
{
	benchmark_mlkem(test, MLKEM768);
}

static void benchmark_mlkem1024(struct kunit *test)
{
	benchmark_mlkem(test, MLKEM1024);
}

/* clang-format off */
static struct kunit_case mlkem_test_cases[] = {
	KUNIT_CASE(test_mlkem768),
	KUNIT_CASE(test_mlkem1024),
	KUNIT_CASE(test_mlkem_reduce),
	KUNIT_CASE(test_mlkem_compress),
	KUNIT_CASE(test_mlkem_decompress),
	KUNIT_CASE(benchmark_mlkem768),
	KUNIT_CASE(benchmark_mlkem1024),
	{},
};
/* clang-format on */

static struct kunit_suite mlkem_test_suite = {
	.name = "mlkem",
	.test_cases = mlkem_test_cases,
};
kunit_test_suite(mlkem_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark for ML-KEM");
MODULE_IMPORT_NS("CRYPTO_INTERNAL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("GPL");
