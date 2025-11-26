/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HKDF: HMAC-based Key Derivation Function (HKDF), RFC 5869
 *
 * Extracted from fs/crypto/hkdf.c, which has
 * Copyright 2019 Google LLC
 */

#ifndef _CRYPTO_HKDF_H
#define _CRYPTO_HKDF_H

#include <crypto/hash.h>

/*
 * HKDF supports any unkeyed cryptographic hash algorithm, but fscrypt uses
 * SHA-512 because it is well-established, secure, and reasonably efficient.
 *
 * HKDF-SHA256 was also considered, as its 256-bit security strength would be
 * sufficient here.  A 512-bit security strength is "nice to have", though.
 * Also, on 64-bit CPUs, SHA-512 is usually just as fast as SHA-256.  In the
 * common case of deriving an AES-256-XTS key (512 bits), that can result in
 * HKDF-SHA512 being much faster than HKDF-SHA256, as the longer digest size of
 * SHA-512 causes HKDF-Expand to only need to do one iteration rather than two.
 */
#define HKDF_HASHLEN            SHA512_DIGEST_SIZE

int hkdf_extract(struct crypto_shash *hmac_tfm, const u8 *ikm,
		 unsigned int ikmlen, const u8 *salt, unsigned int saltlen,
		 u8 *prk);
int hkdf_expand(struct crypto_shash *hmac_tfm,
		const u8 *info, unsigned int infolen,
		u8 *okm, unsigned int okmlen);
#endif
