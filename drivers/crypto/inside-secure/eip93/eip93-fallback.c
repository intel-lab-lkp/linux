// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Jihong Min <hurryman2212@gmail.com>
 */

#include <crypto/fallback.h>
#include <crypto/md5.h>
#include <linux/err.h>
#include <linux/module.h>

#include "eip93-fallback.h"
#include "eip93-main.h"

/*
 * Fixed, non-secret keys used only for benchmark setup. The RFC 3686 key
 * contains a 16-byte AES-128 key followed by its required 4-byte nonce.
 */

static const u8 eip93_fallback_aes_key[] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7,
	0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c, 0x00, 0x00, 0x00, 0x01,
};

static const u8 eip93_fallback_des_key[] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

static const u8 eip93_fallback_3des_key[] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x23, 0x45, 0x67, 0x89,
	0xab, 0xcd, 0xef, 0x01, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0x01, 0x23,
};

static const u8 eip93_fallback_auth_key[] = {
	0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
	0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
};

/* Group EIP93 algorithms by their observed request-size performance trends. */

static const char *const eip93_fallback_raw_aes[] = {
	"ecb(aes-eip93)",
	"cbc(aes-eip93)",
	"ctr(aes-eip93)",
	"rfc3686(ctr(aes-eip93))",
};

static const char *const eip93_fallback_raw_des[] = {
	"ecb(des-eip93)",
	"cbc(des-eip93)",
};

static const char *const eip93_fallback_raw_3des[] = {
	"ecb(des3_ede-eip93)",
	"cbc(des3_ede-eip93)",
};

static const char *const eip93_fallback_hash_md5[] = {
	"md5-eip93",
	"hmac(md5-eip93)",
};

static const char *const eip93_fallback_hash_sha1[] = {
	"sha1-eip93",
	"hmac(sha1-eip93)",
};

static const char *const eip93_fallback_hash_sha2[] = {
	"sha224-eip93",
	"sha256-eip93",
	"hmac(sha224-eip93)",
	"hmac(sha256-eip93)",
};

static const char *const eip93_fallback_authenc_aes[] = {
	"authenc(hmac(md5-eip93), cbc(aes-eip93))",
	"authenc(hmac(sha1-eip93),cbc(aes-eip93))",
	"authenc(hmac(sha224-eip93),cbc(aes-eip93))",
	"authenc(hmac(sha256-eip93),cbc(aes-eip93))",
	"authenc(hmac(md5-eip93),rfc3686(ctr(aes-eip93)))",
	"authenc(hmac(sha1-eip93),rfc3686(ctr(aes-eip93)))",
	"authenc(hmac(sha224-eip93),rfc3686(ctr(aes-eip93)))",
	"authenc(hmac(sha256-eip93),rfc3686(ctr(aes-eip93)))",
};

static const char *const eip93_fallback_authenc_des[] = {
	"authenc(hmac(md5-eip93),cbc(des-eip93))",
	"authenc(hmac(sha1-eip93),cbc(des-eip93))",
	"authenc(hmac(sha224-eip93),cbc(des-eip93))",
	"authenc(hmac(sha256-eip93),cbc(des-eip93))",
};

static const char *const eip93_fallback_authenc_3des[] = {
	"authenc(hmac(md5-eip93),cbc(des3_ede-eip93))",
	"authenc(hmac(sha1-eip93),cbc(des3_ede-eip93))",
	"authenc(hmac(sha224-eip93),cbc(des3_ede-eip93))",
	"authenc(hmac(sha256-eip93),cbc(des3_ede-eip93))",
};

static const struct crypto_fallback_group eip93_fallback_groups[] = {
	CRYPTO_FALLBACK_GROUP("raw_aes", CRYPTO_FALLBACK_SKCIPHER,
			      "rfc3686(ctr(aes))", "rfc3686(ctr(aes-eip93))",
			      eip93_fallback_aes_key, eip93_fallback_raw_aes,
			      ARRAY_SIZE(eip93_fallback_raw_aes)),
	CRYPTO_FALLBACK_GROUP("raw_des", CRYPTO_FALLBACK_SKCIPHER, "ecb(des)",
			      "ecb(des-eip93)", eip93_fallback_des_key,
			      eip93_fallback_raw_des,
			      ARRAY_SIZE(eip93_fallback_raw_des)),
	CRYPTO_FALLBACK_GROUP("raw_3des", CRYPTO_FALLBACK_SKCIPHER,
			      "cbc(des3_ede)", "cbc(des3_ede-eip93)",
			      eip93_fallback_3des_key, eip93_fallback_raw_3des,
			      ARRAY_SIZE(eip93_fallback_raw_3des)),
	CRYPTO_FALLBACK_GROUP("hash_md5", CRYPTO_FALLBACK_AHASH, "hmac(md5)",
			      "hmac(md5-eip93)", eip93_fallback_auth_key,
			      eip93_fallback_hash_md5,
			      ARRAY_SIZE(eip93_fallback_hash_md5)),
	CRYPTO_FALLBACK_GROUP("hash_sha1", CRYPTO_FALLBACK_AHASH, "hmac(sha1)",
			      "hmac(sha1-eip93)", eip93_fallback_auth_key,
			      eip93_fallback_hash_sha1,
			      ARRAY_SIZE(eip93_fallback_hash_sha1)),
	CRYPTO_FALLBACK_GROUP("hash_sha2", CRYPTO_FALLBACK_AHASH,
			      "hmac(sha256)", "hmac(sha256-eip93)",
			      eip93_fallback_auth_key, eip93_fallback_hash_sha2,
			      ARRAY_SIZE(eip93_fallback_hash_sha2)),
	CRYPTO_FALLBACK_GROUP_AUTHENC("authenc_aes",
				      "authenc(hmac(md5),rfc3686(ctr(aes)))",
				      "authenc(hmac(md5-eip93),rfc3686(ctr(aes-eip93)))",
				      eip93_fallback_aes_key,
				      eip93_fallback_auth_key, MD5_DIGEST_SIZE,
				      eip93_fallback_authenc_aes,
				      ARRAY_SIZE(eip93_fallback_authenc_aes)),
	CRYPTO_FALLBACK_GROUP_AUTHENC("authenc_des",
				      "authenc(hmac(md5),cbc(des))",
				      "authenc(hmac(md5-eip93),cbc(des-eip93))",
				      eip93_fallback_des_key,
				      eip93_fallback_auth_key, MD5_DIGEST_SIZE,
				      eip93_fallback_authenc_des,
				      ARRAY_SIZE(eip93_fallback_authenc_des)),
	CRYPTO_FALLBACK_GROUP_AUTHENC("authenc_3des",
				      "authenc(hmac(md5),cbc(des3_ede))",
				      "authenc(hmac(md5-eip93),cbc(des3_ede-eip93))",
				      eip93_fallback_3des_key,
				      eip93_fallback_auth_key, MD5_DIGEST_SIZE,
				      eip93_fallback_authenc_3des,
				      ARRAY_SIZE(eip93_fallback_authenc_3des)),
};

static struct crypto_fallback *eip93_fallback;

int eip93_fallback_register(struct eip93_device *eip93)
{
	eip93_fallback =
		crypto_fallback_register(THIS_MODULE, eip93->dev,
					 eip93_fallback_groups,
					 ARRAY_SIZE(eip93_fallback_groups));

	return PTR_ERR_OR_ZERO(eip93_fallback);
}

void eip93_fallback_unregister(void)
{
	crypto_fallback_unregister(eip93_fallback);

	eip93_fallback = NULL;
}
