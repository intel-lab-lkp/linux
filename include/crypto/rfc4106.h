/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYPTO_RFC4106_H
#define _CRYPTO_RFC4106_H

#include <crypto/aes.h>

#define RFC4106_SALT_SIZE		4

#define RFC4106_AEAD_KEYSIZE_128	(RFC4106_SALT_SIZE + AES_KEYSIZE_128) /* 20 */
#define RFC4106_AEAD_KEYSIZE_192	(RFC4106_SALT_SIZE + AES_KEYSIZE_192) /* 28 */
#define RFC4106_AEAD_KEYSIZE_256	(RFC4106_SALT_SIZE + AES_KEYSIZE_256) /* 36 */

static inline bool rfc4106_keysize_ok(unsigned int keylen)
{
	return keylen == RFC4106_AEAD_KEYSIZE_128 ||
		keylen == RFC4106_AEAD_KEYSIZE_192 ||
		keylen == RFC4106_AEAD_KEYSIZE_256;
}

#endif /* _CRYPTO_RFC4106_H */
