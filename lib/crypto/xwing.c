// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * X-Wing key encapsulation mechanism
 *
 * See include/crypto/xwing.h for the documentation.
 *
 * Copyright 2026 Google LLC
 */

#include <crypto/curve25519.h>
#include <crypto/mlkem.h>
#include <crypto/sha3.h>
#include <crypto/xwing.h>
#include <kunit/visibility.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/slab.h>

/* pk = pk_mlkem || pk_x25519 */
static_assert(XWING_PUBLIC_KEY_BYTES ==
	      MLKEM768_PUBLIC_KEY_BYTES + CURVE25519_KEY_SIZE);

/* sk = seed */
static_assert(XWING_SECRET_KEY_BYTES == XWING_SEED_BYTES);

/* ct = ct_mlkem || ct_x25519 */
static_assert(XWING_CIPHERTEXT_BYTES ==
	      MLKEM768_CIPHERTEXT_BYTES + CURVE25519_KEY_SIZE);

/* expanded_sk = sk_mlkem || sk_x25519 || pk_mlkem || pk_x25519 */
#define XWING_EXPANDED_SECRET_KEY_BYTES                    \
	(MLKEM768_SECRET_KEY_BYTES + CURVE25519_KEY_SIZE + \
	 MLKEM768_PUBLIC_KEY_BYTES + CURVE25519_KEY_SIZE)

static int xwing_expand_sk(u8 expanded_sk[XWING_EXPANDED_SECRET_KEY_BYTES],
			   const u8 sk[XWING_SECRET_KEY_BYTES])
{
	u8 *sk_mlkem = &expanded_sk[0];
	u8 *sk_x25519 = &sk_mlkem[MLKEM768_SECRET_KEY_BYTES];
	u8 *pk_mlkem = &sk_x25519[CURVE25519_KEY_SIZE];
	u8 *pk_x25519 = &pk_mlkem[MLKEM768_PUBLIC_KEY_BYTES];
	u8 seed_mlkem[MLKEM_SEED_BYTES];
	struct shake_ctx shake;
	int err;

	shake256_init(&shake);
	shake_update(&shake, sk, XWING_SECRET_KEY_BYTES);
	shake_squeeze(&shake, seed_mlkem, sizeof(seed_mlkem));

	err = mlkem768_keygen_internal(pk_mlkem, sk_mlkem, seed_mlkem);
	if (err) /* can only be -ENOMEM */
		goto out;
	shake_squeeze(&shake, sk_x25519, CURVE25519_KEY_SIZE);
	curve25519_clamp_secret(sk_x25519);
	if (unlikely(!curve25519_generate_public(pk_x25519, sk_x25519)))
		err = -EAGAIN;
out:
	shake_zeroize_ctx(&shake);
	memzero_explicit(seed_mlkem, sizeof(seed_mlkem));
	return err;
}

VISIBLE_IF_KUNIT int xwing_keygen_internal(u8 pk[XWING_PUBLIC_KEY_BYTES],
					   u8 sk[XWING_SECRET_KEY_BYTES],
					   const u8 seed[XWING_SEED_BYTES])
{
	u8 *expanded_sk __free(kfree_sensitive) =
		kmalloc(XWING_EXPANDED_SECRET_KEY_BYTES, GFP_KERNEL);
	int err;

	if (!expanded_sk)
		return -ENOMEM;

	err = xwing_expand_sk(expanded_sk, seed);
	if (err)
		return err;
	/* pk = pk_mlkem || pk_x25519 */
	memcpy(pk,
	       &expanded_sk[MLKEM768_SECRET_KEY_BYTES + CURVE25519_KEY_SIZE],
	       XWING_PUBLIC_KEY_BYTES);
	/* sk = seed */
	memcpy(sk, seed, XWING_SECRET_KEY_BYTES);
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(xwing_keygen_internal);

int xwing_keygen(u8 pk[XWING_PUBLIC_KEY_BYTES], u8 sk[XWING_SECRET_KEY_BYTES])
{
	u8 seed[XWING_SEED_BYTES];
	int err;

	do {
		get_random_bytes(seed, sizeof(seed));
		err = xwing_keygen_internal(pk, sk, seed);
	} while (err == -EAGAIN); /* curve25519_null_point case */
	memzero_explicit(seed, sizeof(seed));
	return err;
}
EXPORT_SYMBOL_GPL(xwing_keygen);

static void xwing_combine(u8 ss[XWING_SHARED_SECRET_BYTES],
			  const u8 ss_mlkem[MLKEM_SHARED_SECRET_BYTES],
			  const u8 ss_x25519[CURVE25519_KEY_SIZE],
			  const u8 ct_x25519[CURVE25519_KEY_SIZE],
			  const u8 pk_x25519[CURVE25519_KEY_SIZE])
{
	static const u8 xwing_label[6] = { 0x5c, 0x2e, 0x2f, 0x2f, 0x5e, 0x5c };
	struct sha3_ctx ctx;

	sha3_256_init(&ctx);
	sha3_update(&ctx, ss_mlkem, MLKEM_SHARED_SECRET_BYTES);
	sha3_update(&ctx, ss_x25519, CURVE25519_KEY_SIZE);
	sha3_update(&ctx, ct_x25519, CURVE25519_KEY_SIZE);
	sha3_update(&ctx, pk_x25519, CURVE25519_KEY_SIZE);
	sha3_update(&ctx, xwing_label, sizeof(xwing_label));
	sha3_final(&ctx, ss);
}

VISIBLE_IF_KUNIT int xwing_encaps_internal(u8 ct[XWING_CIPHERTEXT_BYTES],
					   u8 ss[XWING_SHARED_SECRET_BYTES],
					   const u8 pk[XWING_PUBLIC_KEY_BYTES],
					   const u8 eseed[XWING_ESEED_BYTES])
{
	const u8 *pk_mlkem = &pk[0];
	const u8 *pk_x25519 = &pk[MLKEM768_PUBLIC_KEY_BYTES];
	const u8 *eseed_mlkem = &eseed[0];
	const u8 *eseed_x25519 = &eseed[MLKEM_ESEED_BYTES];
	u8 eph_sk_x25519[CURVE25519_KEY_SIZE];
	u8 *ct_mlkem = &ct[0];
	u8 *ct_x25519 = &ct[MLKEM768_CIPHERTEXT_BYTES];
	u8 ss_mlkem[MLKEM_SHARED_SECRET_BYTES];
	u8 ss_x25519[CURVE25519_KEY_SIZE];
	int err;

	err = mlkem768_encaps_internal(ct_mlkem, ss_mlkem, pk_mlkem,
				       eseed_mlkem);
	if (err)
		goto out;
	memcpy(eph_sk_x25519, eseed_x25519, CURVE25519_KEY_SIZE);
	curve25519_clamp_secret(eph_sk_x25519);
	if (!curve25519_generate_public(ct_x25519, eph_sk_x25519)) {
		err = -EAGAIN;
		goto out;
	}
	if (!curve25519(ss_x25519, eph_sk_x25519, pk_x25519)) {
		err = -EBADMSG;
		goto out;
	}
	xwing_combine(ss, ss_mlkem, ss_x25519, ct_x25519, pk_x25519);
	err = 0;
out:
	if (err) {
		get_random_bytes(ct, XWING_CIPHERTEXT_BYTES);
		get_random_bytes(ss, XWING_SHARED_SECRET_BYTES);
	}
	memzero_explicit(eph_sk_x25519, sizeof(eph_sk_x25519));
	memzero_explicit(ss_mlkem, sizeof(ss_mlkem));
	memzero_explicit(ss_x25519, sizeof(ss_x25519));
	return err;
}
EXPORT_SYMBOL_IF_KUNIT(xwing_encaps_internal);

int xwing_encaps(u8 ct[XWING_CIPHERTEXT_BYTES],
		 u8 ss[XWING_SHARED_SECRET_BYTES],
		 const u8 pk[XWING_PUBLIC_KEY_BYTES])
{
	u8 eseed[XWING_ESEED_BYTES];
	int err;

	do {
		get_random_bytes(eseed, sizeof(eseed));
		err = xwing_encaps_internal(ct, ss, pk, eseed);
	} while (err == -EAGAIN); /* curve25519_null_point case */
	memzero_explicit(eseed, sizeof(eseed));
	return err;
}
EXPORT_SYMBOL_GPL(xwing_encaps);

int xwing_decaps(u8 ss[XWING_SHARED_SECRET_BYTES],
		 const u8 ct[XWING_CIPHERTEXT_BYTES],
		 const u8 sk[XWING_SECRET_KEY_BYTES])
{
	u8 *expanded_sk __free(kfree_sensitive) =
		kmalloc(XWING_EXPANDED_SECRET_KEY_BYTES, GFP_KERNEL);
	u8 *sk_mlkem, *sk_x25519, *pk_mlkem, *pk_x25519;
	const u8 *ct_mlkem = &ct[0];
	const u8 *ct_x25519 = &ct[MLKEM768_CIPHERTEXT_BYTES];
	u8 ss_mlkem[MLKEM_SHARED_SECRET_BYTES];
	u8 ss_x25519[CURVE25519_KEY_SIZE];
	int err;

	if (!expanded_sk) {
		err = -ENOMEM;
		goto out;
	}
	err = xwing_expand_sk(expanded_sk, sk);
	if (err) {
		if (err == -EAGAIN) /* curve25519_null_point case */
			err = -EBADMSG;
		goto out;
	}
	sk_mlkem = &expanded_sk[0];
	sk_x25519 = &sk_mlkem[MLKEM768_SECRET_KEY_BYTES];
	pk_mlkem = &sk_x25519[CURVE25519_KEY_SIZE];
	pk_x25519 = &pk_mlkem[MLKEM768_PUBLIC_KEY_BYTES];

	err = mlkem768_decaps(ss_mlkem, ct_mlkem, sk_mlkem);
	if (err) {
		/*
		 * This is either -ENOMEM, or -EBADMSG for a malformed secret
		 * key.  This case is *not* reached if the ciphertext is
		 * invalid, as implicit rejection is used.
		 */
		goto out;
	}
	if (!curve25519(ss_x25519, sk_x25519, ct_x25519)) {
		/*
		 * ss_x25519 is curve25519_null_point, which can happen if the
		 * ciphertext is invalid.  In this case the correct behavior is
		 * to continue anyway and implicitly reject.
		 */
	}

	xwing_combine(ss, ss_mlkem, ss_x25519, ct_x25519, pk_x25519);
	err = 0;
out:
	if (err)
		get_random_bytes(ss, XWING_SHARED_SECRET_BYTES);
	memzero_explicit(ss_mlkem, sizeof(ss_mlkem));
	memzero_explicit(ss_x25519, sizeof(ss_x25519));
	return err;
}
EXPORT_SYMBOL_GPL(xwing_decaps);

MODULE_DESCRIPTION("X-Wing key encapsulation mechanism");
MODULE_IMPORT_NS("CRYPTO_INTERNAL");
MODULE_LICENSE("GPL");
