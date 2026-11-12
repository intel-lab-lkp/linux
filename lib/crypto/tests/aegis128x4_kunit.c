// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit test suite for AEGIS-128X4
 *
 * Copyright 2026 Antonio Quartulli <antonio@mandelbit.com>
 */
#include <crypto/aegis128x4.h>
#include <crypto/blake2s.h>
#include "test-utils.h"

/* AEGIS-128X4 test vectors from RFC 10032, Appendix A.5 */
static const struct aegis128x4_testvec {
	const char *name;
	const char *key;
	const char *nonce;
	const char *ad;
	size_t ad_len;
	const char *ptext;
	const char *ctext;
	size_t data_len;
	const char *tag;
	size_t tag_len;
} aegis128x4_testvecs[] = {
	{
		.name = "RFC 10032 A.5.2 (128-bit tag)",
		.key = "\x00\x01\x02\x03\x04\x05\x06\x07"
		       "\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f",
		.nonce = "\x10\x11\x12\x13\x14\x15\x16\x17"
			 "\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f",
		.ad = "",
		.ad_len = 0,
		.ptext = "",
		.ctext = "",
		.data_len = 0,
		.tag = "\x5b\xef\x76\x2d\x09\x47\xc0\x04"
		       "\x55\xb9\x7b\xb3\xaf\x30\xdf\xa3",
		.tag_len = 16,
	},
	{
		.name = "RFC 10032 A.5.2 (256-bit tag)",
		.key = "\x00\x01\x02\x03\x04\x05\x06\x07"
		       "\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f",
		.nonce = "\x10\x11\x12\x13\x14\x15\x16\x17"
			 "\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f",
		.ad = "",
		.ad_len = 0,
		.ptext = "",
		.ctext = "",
		.data_len = 0,
		.tag = "\xa4\xb2\x54\x37\xf4\xbe\x93\xcf"
		       "\xa8\x56\xa2\xf2\x7e\x44\x16\xb4"
		       "\x2c\xac\x79\xfd\x46\x98\xf2\xcd"
		       "\xbe\x6a\xf2\x56\x73\xe1\x0a\x68",
		.tag_len = 32,
	},
	{
		.name = "RFC 10032 A.5.3 (128-bit tag)",
		.key = "\x00\x01\x02\x03\x04\x05\x06\x07"
		       "\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f",
		.nonce = "\x10\x11\x12\x13\x14\x15\x16\x17"
			 "\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f",
		.ad = "\x01\x02\x03\x04\x01\x02\x03\x04",
		.ad_len = 8,
		.ptext = "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07",
		.ctext = "\xe8\x36\x11\x85\x62\xf4\x47\x9c"
			 "\x9d\x35\xc1\x73\x56\xa8\x33\x11"
			 "\x4c\x21\xf9\xaa\x39\xe4\xdd\xa5"
			 "\xe5\xc8\x7f\x41\x52\xa0\x0f\xce"
			 "\x9a\x7c\x38\xf8\x32\xea\xfe\x8b"
			 "\x1c\x12\xf8\xa7\xcf\x12\xa8\x1a"
			 "\x1a\xd8\xa9\xc2\x4b\xa9\xde\xdf"
			 "\xbd\xaa\x58\x6f\xfe\xa6\x7d\xdc"
			 "\x80\x1e\xa9\x7d\x9a\xb4\xa8\x72"
			 "\xf4\x2d\x0e\x35\x2e\x27\x13\xda"
			 "\xcd\x60\x9f\x94\x42\xc1\x75\x17"
			 "\xc5\xa2\x9d\xaf\x3e\x2a\x3f\xac"
			 "\x4f\xf6\xb1\x38\x0c\x4e\x46\xdf"
			 "\x7b\x08\x6a\xf6\xce\x6b\xc1\xed"
			 "\x59\x4b\x8d\xd6\x4a\xed\x2a\x7e",
		.data_len = 120,
		.tag = "\x0e\x56\xab\x94\xe2\xe8\x5d\xb8"
		       "\x0f\x9d\x54\x01\x0c\xaa\xbf\xb4",
		.tag_len = 16,
	},
	{
		.name = "RFC 10032 A.5.3 (256-bit tag)",
		.key = "\x00\x01\x02\x03\x04\x05\x06\x07"
		       "\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f",
		.nonce = "\x10\x11\x12\x13\x14\x15\x16\x17"
			 "\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f",
		.ad = "\x01\x02\x03\x04\x01\x02\x03\x04",
		.ad_len = 8,
		.ptext = "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07"
			 "\x04\x05\x06\x07\x04\x05\x06\x07",
		.ctext = "\xe8\x36\x11\x85\x62\xf4\x47\x9c"
			 "\x9d\x35\xc1\x73\x56\xa8\x33\x11"
			 "\x4c\x21\xf9\xaa\x39\xe4\xdd\xa5"
			 "\xe5\xc8\x7f\x41\x52\xa0\x0f\xce"
			 "\x9a\x7c\x38\xf8\x32\xea\xfe\x8b"
			 "\x1c\x12\xf8\xa7\xcf\x12\xa8\x1a"
			 "\x1a\xd8\xa9\xc2\x4b\xa9\xde\xdf"
			 "\xbd\xaa\x58\x6f\xfe\xa6\x7d\xdc"
			 "\x80\x1e\xa9\x7d\x9a\xb4\xa8\x72"
			 "\xf4\x2d\x0e\x35\x2e\x27\x13\xda"
			 "\xcd\x60\x9f\x94\x42\xc1\x75\x17"
			 "\xc5\xa2\x9d\xaf\x3e\x2a\x3f\xac"
			 "\x4f\xf6\xb1\x38\x0c\x4e\x46\xdf"
			 "\x7b\x08\x6a\xf6\xce\x6b\xc1\xed"
			 "\x59\x4b\x8d\xd6\x4a\xed\x2a\x7e",
		.data_len = 120,
		.tag = "\x69\xab\xf0\xf6\x4a\x13\x7d\xd6"
		       "\xe1\x22\x47\x8d\x77\x7e\x98\xbc"
		       "\x42\x28\x23\x00\x6c\xf5\x7f\x5e"
		       "\xe8\x22\xdd\x78\x39\x72\x30\xb2",
		.tag_len = 32,
	},
};

static void test_aegis128x4_one_test_vector(struct kunit *test,
					    const struct aegis128x4_testvec *tv)
{
	u8 *ctext = alloc_buf(test, tv->data_len);
	u8 *decrypted = alloc_buf(test, tv->data_len);
	u8 *tag = alloc_buf(test, tv->tag_len);
	struct aegis128x4_key key;
	int err;

	err = aegis128x4_preparekey(&key, tv->key, AEGIS128X4_KEY_SIZE,
				    tv->tag_len);
	KUNIT_ASSERT_EQ_MSG(test, 0, err, "Failed to prepare key for %s",
			    tv->name);

	aegis128x4_encrypt(ctext, tv->ptext, tv->data_len, tag, tv->ad,
			   tv->ad_len, tv->nonce, &key);
	KUNIT_ASSERT_MEMEQ_MSG(test, tv->ctext, ctext, tv->data_len,
			       "Wrong ciphertext for %s", tv->name);
	KUNIT_ASSERT_MEMEQ_MSG(test, tag, tv->tag, tv->tag_len,
			       "Wrong tag for %s", tv->name);

	err = aegis128x4_decrypt(decrypted, ctext, tv->data_len, tag, tv->ad,
				 tv->ad_len, tv->nonce, &key);
	KUNIT_ASSERT_EQ_MSG(test, 0, err, "Decryption failed for %s", tv->name);
	KUNIT_ASSERT_MEMEQ_MSG(test, tv->ptext, decrypted, tv->data_len,
			       "Wrong plaintext for %s", tv->name);
}

static void test_aegis128x4_test_vectors(struct kunit *test)
{
	for (size_t i = 0; i < ARRAY_SIZE(aegis128x4_testvecs); i++)
		test_aegis128x4_one_test_vector(test, &aegis128x4_testvecs[i]);
}

/* The (data, associated data) lengths of AEGIS128X4_LONG_LENS in the script */
static const struct {
	size_t data_len;
	size_t ad_len;
} aegis128x4_long_lens[] = {
	{ 4097, 0 },
	{ 8320, 4100 },
	{ 12345, 129 },
};

/* This value was generated by gen-aead-testvecs.py. */
static const u8 aegis128x4_long_checksum[BLAKE2S_HASH_SIZE] = {
	0x0a, 0x18, 0x21, 0x25, 0x48, 0xe7, 0x99, 0x53, 0x09, 0x00, 0x0d,
	0xf5, 0x61, 0x1b, 0xa4, 0x11, 0xe1, 0x91, 0xef, 0xf5, 0x60, 0x84,
	0x79, 0x26, 0x4c, 0x20, 0xb2, 0x0d, 0x22, 0x70, 0x10, 0xfb,
};

/*
 * Test messages longer than the 4 KiB that the x86 code processes at a time,
 * out-of-place, in-place and incrementally in pieces of random length.
 */
static void test_aegis128x4_long(struct kunit *test)
{
	const size_t max_len = 12345;
	u8 *pt = alloc_buf(test, max_len);
	u8 *ad = alloc_buf(test, max_len);
	u8 *ct = alloc_buf(test, max_len);
	u8 *buf = alloc_buf(test, max_len);
	u8 tag[AEGIS128X4_MAX_AUTHTAG_SIZE], tag2[AEGIS128X4_MAX_AUTHTAG_SIZE];
	u8 raw_key[AEGIS128X4_KEY_SIZE], nonce[AEGIS128X4_NONCE_SIZE];
	u8 checksum[BLAKE2S_HASH_SIZE];
	struct blake2s_ctx checksum_ctx;
	struct aegis128x4_key key;
	struct aegis128x4_ctx ctx;
	size_t pos, n;
	int err;

	blake2s_init(&checksum_ctx, BLAKE2S_HASH_SIZE);
	for (size_t i = 0; i < ARRAY_SIZE(aegis128x4_long_lens); i++) {
		size_t data_len = aegis128x4_long_lens[i].data_len;
		size_t ad_len = aegis128x4_long_lens[i].ad_len;

		rand_bytes_seeded_from_len(pt, data_len);
		rand_bytes_seeded_from_len(ad, ad_len);
		rand_bytes_seeded_from_len(raw_key, sizeof(raw_key));
		rand_bytes_seeded_from_len(nonce, sizeof(nonce));
		err = aegis128x4_preparekey(&key, raw_key, sizeof(raw_key),
					    sizeof(tag));
		KUNIT_ASSERT_EQ(test, 0, err);

		aegis128x4_encrypt(ct, pt, data_len, tag, ad, ad_len, nonce,
				   &key);
		blake2s_update(&checksum_ctx, ct, data_len);
		blake2s_update(&checksum_ctx, tag, sizeof(tag));

		memcpy(buf, pt, data_len);
		aegis128x4_encrypt(buf, buf, data_len, tag2, ad, ad_len, nonce,
				   &key);
		KUNIT_ASSERT_MEMEQ(test, ct, buf, data_len);
		KUNIT_ASSERT_MEMEQ(test, tag, tag2, sizeof(tag));

		aegis128x4_init(&ctx, nonce, &key);
		for (pos = 0; pos < ad_len; pos += n) {
			n = min(ad_len - pos, 1 + rand_length(1000));
			aegis128x4_auth_update(&ctx, &ad[pos], n);
		}
		for (pos = 0; pos < data_len; pos += n) {
			n = min(data_len - pos, 1 + rand_length(5000));
			aegis128x4_encrypt_update(&ctx, &buf[pos], &pt[pos], n);
		}
		aegis128x4_encrypt_final(&ctx, tag2);
		KUNIT_ASSERT_MEMEQ(test, ct, buf, data_len);
		KUNIT_ASSERT_MEMEQ(test, tag, tag2, sizeof(tag));

		memcpy(buf, ct, data_len);
		err = aegis128x4_decrypt(buf, buf, data_len, tag, ad, ad_len,
					 nonce, &key);
		KUNIT_ASSERT_EQ(test, 0, err);
		KUNIT_ASSERT_MEMEQ(test, pt, buf, data_len);
	}
	blake2s_final(&checksum_ctx, checksum);
	KUNIT_EXPECT_MEMEQ(test, checksum, aegis128x4_long_checksum,
			   BLAKE2S_HASH_SIZE);
}

static int aegis128x4_init_test(struct aegis128x4_ctx *ctx, u64 data_len,
				u64 ad_len, const u8 *nonce, size_t nonce_len,
				const struct aegis128x4_key *key)
{
	if (nonce_len != AEGIS128X4_NONCE_SIZE)
		return -EINVAL;
	/*
	 * Ignore data_len and ad_len.  Incremental AEGIS-128X4 doesn't need
	 * them at initialization time.
	 */
	aegis128x4_init(ctx, nonce, key);
	return 0;
}

static int aegis128x4_encrypt_test(u8 *dst, const u8 *src, size_t data_len,
				   u8 *authtag, const u8 *ad, size_t ad_len,
				   const u8 *nonce, size_t nonce_len,
				   const struct aegis128x4_key *key)
{
	if (nonce_len != AEGIS128X4_NONCE_SIZE)
		return -EINVAL;
	/* aegis128x4_encrypt() returns void. */
	aegis128x4_encrypt(dst, src, data_len, authtag, ad, ad_len, nonce, key);
	return 0;
}

static int aegis128x4_decrypt_test(u8 *dst, const u8 *src, size_t data_len,
				   const u8 *authtag, const u8 *ad,
				   size_t ad_len, const u8 *nonce,
				   size_t nonce_len,
				   const struct aegis128x4_key *key)
{
	if (nonce_len != AEGIS128X4_NONCE_SIZE)
		return -EINVAL;
	return aegis128x4_decrypt(dst, src, data_len, authtag, ad, ad_len,
				  nonce, key);
}

static const size_t aegis128x4_valid_key_lens[] = { AEGIS128X4_KEY_SIZE };
#define AEAD_MAX_KEY_LEN AEGIS128X4_KEY_SIZE
#define AEAD_VALID_KEY_LENS aegis128x4_valid_key_lens

static const size_t aegis128x4_valid_nonce_lens[] = { AEGIS128X4_NONCE_SIZE };
#define AEAD_MAX_NONCE_LEN AEGIS128X4_NONCE_SIZE
#define AEAD_VALID_NONCE_LENS aegis128x4_valid_nonce_lens

static const size_t aegis128x4_valid_tag_lens[] = { 16, 32 };
#define AEAD_MAX_TAG_LEN AEGIS128X4_MAX_AUTHTAG_SIZE
#define AEAD_VALID_TAG_LENS aegis128x4_valid_tag_lens

#define AEAD_KEY aegis128x4_key
#define AEAD_CTX aegis128x4_ctx
#define AEAD_PREPAREKEY aegis128x4_preparekey
#define AEAD_ENCRYPT aegis128x4_encrypt_test
#define AEAD_DECRYPT aegis128x4_decrypt_test

#define AEAD_INIT aegis128x4_init_test
#define AEAD_AUTH_UPDATE aegis128x4_auth_update
#define AEAD_ENCRYPT_UPDATE aegis128x4_encrypt_update
#define AEAD_DECRYPT_UPDATE aegis128x4_decrypt_update
#define AEAD_ENCRYPT_FINAL aegis128x4_encrypt_final
#define AEAD_DECRYPT_FINAL aegis128x4_decrypt_final

/* This value was generated by gen-aead-testvecs.py. */
static const u8 aegis128x4_monte_carlo_checksum[BLAKE2S_HASH_SIZE] = {
	0x57, 0xf2, 0x44, 0xb4, 0x43, 0x71, 0x7c, 0x34, 0x94, 0x93, 0x22,
	0xaa, 0xaf, 0x75, 0x5b, 0xa9, 0x5c, 0x23, 0x26, 0xbb, 0x4f, 0xf2,
	0x61, 0x43, 0xa6, 0x92, 0xc7, 0x46, 0x3a, 0x17, 0x58, 0x67,
};
#define AEAD_MONTE_CARLO_CHECKSUM aegis128x4_monte_carlo_checksum

#include "aead-test-template.h"

static struct kunit_case aegis128x4_test_cases[] = {
	KUNIT_CASE(test_aegis128x4_test_vectors),
	KUNIT_CASE(test_aegis128x4_long),
	AEAD_KUNIT_CASES,
	{},
};

static struct kunit_suite aegis128x4_test_suite = {
	.name = "aegis128x4",
	.test_cases = aegis128x4_test_cases,
};
kunit_test_suite(aegis128x4_test_suite);

MODULE_DESCRIPTION("KUnit tests and benchmark for AEGIS-128X4");
MODULE_LICENSE("GPL");
