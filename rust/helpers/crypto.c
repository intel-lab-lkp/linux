// SPDX-License-Identifier: GPL-2.0

#include <crypto/akcipher.h>
#include <crypto/aes.h>
#include <crypto/aes-cbc-macs.h>
#include <linux/string.h>

__rust_helper void rust_helper_memzero_explicit(void *s, size_t count)
{
	memzero_explicit(s, count);
}

#ifdef CONFIG_RUST_CRYPTO_AKCIPHER
__rust_helper void rust_helper_crypto_free_akcipher(struct crypto_akcipher *tfm)
{
	crypto_free_akcipher(tfm);
}

__rust_helper int
rust_helper_crypto_akcipher_set_pub_key(struct crypto_akcipher *tfm,
					const void *key, unsigned int key_len)
{
	return crypto_akcipher_set_pub_key(tfm, key, key_len);
}
#endif

#ifdef CONFIG_RUST_CRYPTO_LIB_AES
/*
 * aes_encrypt() takes a transparent union (aes_encrypt_arg) that bindgen cannot
 * express, so the single-block encrypt step is wrapped here. The key schedule
 * is prepared once (aes_prepareenckey() is a plain extern bound directly) and
 * the resulting struct aes_enckey is reused across blocks by the caller, so the
 * key is not re-expanded per block. SHA-256 and HMAC-SHA256 are plain extern
 * functions and are bound directly.
 */
__rust_helper void
rust_helper_aes_enckey_encrypt_block(const struct aes_enckey *key, u8 *out,
				     const u8 *in)
{
	aes_encrypt(key, out, in);
}

__rust_helper void rust_helper_aes_enckey_zero(struct aes_enckey *key)
{
	memzero_explicit(key, sizeof(*key));
}

/*
 * AES-CMAC one-shot over the in-tree library (crypto/aes-cbc-macs.h): prepares
 * the 128-bit key, MACs @data and writes the 16-byte tag to @out. A helper
 * because both aes_cmac_preparekey()'s struct and the aes_cmac() one-shot are
 * not expressible from Rust directly. The key length is fixed at 128 bits, so
 * aes_cmac_preparekey() cannot fail; the prepared key is wiped before return.
 */
__rust_helper void
rust_helper_aes_cmac(const u8 *key, const u8 *data, size_t data_len, u8 *out)
{
	struct aes_cmac_key cmac_key;

	aes_cmac_preparekey(&cmac_key, key, AES_KEYSIZE_128);
	aes_cmac(&cmac_key, data, data_len, out);
	memzero_explicit(&cmac_key, sizeof(cmac_key));
}
#endif
