// SPDX-License-Identifier: GPL-2.0

#include <crypto/aes.h>
#include <linux/string.h>

/*
 * AES-128 single-block ECB encryption: out = AES(key, in).
 *
 * A helper because aes_encrypt() takes a transparent union (aes_encrypt_arg)
 * that bindgen cannot express. SHA-256 and HMAC-SHA256 are plain extern
 * functions and are bound directly.
 */
__rust_helper int
rust_helper_aes128_encrypt_block(const u8 *key, const u8 *in, u8 *out)
{
	struct aes_enckey enckey;
	int ret;

	ret = aes_prepareenckey(&enckey, key, AES_KEYSIZE_128);
	if (ret)
		return ret;
	aes_encrypt(&enckey, out, in);
	memzero_explicit(&enckey, sizeof(enckey));
	return 0;
}
