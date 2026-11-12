/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AEGIS-128X4 authenticated encryption and decryption (RFC 10032)
 *
 * Copyright 2026 Antonio Quartulli <antonio@mandelbit.com>
 */
#ifndef _CRYPTO_AEGIS128X4_H
#define _CRYPTO_AEGIS128X4_H

#include <linux/string.h>
#include <linux/types.h>

#define AEGIS128X4_KEY_SIZE	16
#define AEGIS128X4_NONCE_SIZE	16
#define AEGIS128X4_MAX_AUTHTAG_SIZE	32

/* Number of parallel AEGIS-128L instances */
#define AEGIS128X4_LANES	4

/* Bytes absorbed and en/decrypted by each state update */
#define AEGIS128X4_BLOCK_SIZE	(32 * AEGIS128X4_LANES)

/**
 * struct aegis128x4_key - A key prepared for AEGIS-128X4
 */
struct aegis128x4_key {
	/* private: */
	u8 key[AEGIS128X4_KEY_SIZE];
	size_t authtag_len; /* Length of authentication tags in bytes */
};

/**
 * aegis128x4_zeroize_key() - Zeroize an aegis128x4_key structure
 * @key: The aegis128x4_key to zeroize
 */
static inline void aegis128x4_zeroize_key(struct aegis128x4_key *key)
{
	memzero_explicit(key, sizeof(*key));
}

/*
 * The state: eight words, each holding the corresponding 16-byte block of the
 * AEGIS-128L instance i at byte offset 16 * i.
 */
union aegis128x4_state {
	u8 b[8][16 * AEGIS128X4_LANES];
	__le32 w32[8][4 * AEGIS128X4_LANES];
	u64 w64[8][2 * AEGIS128X4_LANES];
};

/**
 * struct aegis128x4_ctx - Context for incrementally en/decrypting a message
 */
struct aegis128x4_ctx {
	/* private: */
	union aegis128x4_state state;
	/*
	 * The associated data or the plaintext of the incomplete block, which
	 * is absorbed once complete or at the end of its phase
	 */
	u8 buf[AEGIS128X4_BLOCK_SIZE] __aligned(__alignof__(u64));
	/*
	 * Pointer to the key, which is assumed to live at least as long as this
	 * struct.
	 */
	const struct aegis128x4_key *key;
	/* Number of associated data bytes processed so far */
	u64 ad_len;
	/* Number of en/decrypted bytes processed so far */
	u64 data_len;
	/* Whether the associated data was absorbed and en/decryption began */
	bool data;
};

/**
 * aegis128x4_zeroize_ctx() - Zeroize an aegis128x4_ctx structure
 * @ctx: The aegis128x4_ctx to zeroize
 */
static inline void aegis128x4_zeroize_ctx(struct aegis128x4_ctx *ctx)
{
	memzero_explicit(ctx, sizeof(*ctx));
}

/**
 * aegis128x4_preparekey() - Prepare a key for AEGIS-128X4
 * @key: (output) The key structure to initialize
 * @in_key: The raw AEGIS-128X4 key
 * @key_len: Length of the raw key in bytes: 16
 * @authtag_len: Length of the authentication tag in bytes: 16 or 32
 *
 * Users should use aegis128x4_zeroize_key() to zeroize the key struct at the
 * end of its lifetime.  (But if this function fails, zeroization is
 * unnecessary.)
 *
 * Context: Any context.
 * Return:
 * * 0 on success
 * * -EINVAL if either of the lengths is invalid
 */
int __must_check aegis128x4_preparekey(struct aegis128x4_key *key,
				       const u8 *in_key, size_t key_len,
				       size_t authtag_len);

/**
 * aegis128x4_encrypt() - Encrypt a message with AEGIS-128X4
 * @dst: The destination ciphertext data.  Can be in-place or out-of-place.
 *	 For other overlaps the behavior is unspecified.
 * @src: The source plaintext data
 * @data_len: Length of plaintext in bytes (and ciphertext excluding the tag):
 *	      less than 2^61
 * @authtag: The output authentication tag.  Length is the authtag_len that was
 *	     passed to aegis128x4_preparekey().  Usually protocols put the tag at
 *	     the end of the ciphertext, in which case this should be set to
 *	     @dst + @data_len and @dst must have room for the tag.
 * @ad: The associated data
 * @ad_len: Length of associated data in bytes: less than 2^61
 * @nonce: The 16-byte nonce.  All (key, nonce) pairs used MUST be distinct.
 * @key: The key, already prepared using aegis128x4_preparekey()
 *
 * Context: Any context.
 */
void aegis128x4_encrypt(u8 *dst, const u8 *src, size_t data_len, u8 *authtag,
			const u8 *ad, size_t ad_len,
			const u8 nonce[at_least AEGIS128X4_NONCE_SIZE],
			const struct aegis128x4_key *key);

/**
 * aegis128x4_decrypt() - Decrypt a message with AEGIS-128X4
 * @dst: The destination plaintext data.  Can be in-place or out-of-place.
 *	 For other overlaps the behavior is unspecified.
 * @src: The source ciphertext data
 * @data_len: Length of plaintext in bytes (and ciphertext excluding the tag):
 *	      less than 2^61
 * @authtag: The stored authentication tag.  Length is the authtag_len that was
 *	     passed to aegis128x4_preparekey().  Usually protocols put the tag at
 *	     the end of the ciphertext, in which case this should be set to
 *	     @src + @data_len.
 * @ad: The associated data
 * @ad_len: Length of associated data in bytes: less than 2^61
 * @nonce: The 16-byte nonce
 * @key: The key, already prepared using aegis128x4_preparekey()
 *
 * Context: Any context.
 * Return:
 * * 0 on success.  This is the only case where any decrypted or associated data
 *   can be used.
 * * -EBADMSG if the message is inauthentic
 */
int __must_check aegis128x4_decrypt(u8 *dst, const u8 *src, size_t data_len,
				    const u8 *authtag, const u8 *ad,
				    size_t ad_len,
				    const u8 nonce[at_least AEGIS128X4_NONCE_SIZE],
				    const struct aegis128x4_key *key);

/**
 * aegis128x4_init() - Initialize context for incremental AEGIS-128X4
 *		       encryption or decryption
 * @ctx: The context to initialize
 * @nonce: The 16-byte nonce.  All (key, nonce) pairs used for encryption MUST
 *	   be distinct.
 * @key: The key, already prepared using aegis128x4_preparekey().  Note that a
 *	 pointer to the key is saved in the context, so the key must live at
 *	 least as long as the context.
 *
 * The context should be zeroized at the end of its lifetime.  Normally that
 * happens in aegis128x4_encrypt_final() or aegis128x4_decrypt_final(), but
 * callers that abandon a context without finalizing it should explicitly
 * zeroize it.
 *
 * IMPORTANT: Callers that are decrypting data MUST NOT assume that any
 * decrypted or associated data is authentic until the authentication tag has
 * been verified.  This incremental API is provided solely to support callers
 * that can't efficiently use the one-shot functions due to using a nonlinear
 * data layout.
 *
 * For incremental AEGIS-128X4 encryption, use:
 *
 * 1. aegis128x4_init()
 * 2. aegis128x4_auth_update() (any number of times)
 * 3. aegis128x4_encrypt_update() (any number of times)
 * 4. aegis128x4_encrypt_final()
 *
 * For incremental AEGIS-128X4 decryption, use:
 *
 * 1. aegis128x4_init()
 * 2. aegis128x4_auth_update() (any number of times)
 * 3. aegis128x4_decrypt_update() (any number of times)
 * 4. aegis128x4_decrypt_final()
 *
 * Context: Any context.
 */
void aegis128x4_init(struct aegis128x4_ctx *ctx,
		     const u8 nonce[at_least AEGIS128X4_NONCE_SIZE],
		     const struct aegis128x4_key *key);

/**
 * aegis128x4_auth_update() - Incrementally process AEGIS-128X4 associated data
 * @ctx: An AEGIS-128X4 context
 * @ad: The associated data
 * @len: Number of bytes provided.  The caller must ensure that the total
 *	 associated data length stays below 2^61.
 *
 * IMPORTANT: Callers MUST NOT assume that any decrypted or associated data is
 * authentic until the authentication tag has been verified.
 *
 * Context: Any context.
 */
void aegis128x4_auth_update(struct aegis128x4_ctx *ctx, const u8 *ad,
			    size_t len);

/**
 * aegis128x4_encrypt_update() - Incrementally encrypt data with AEGIS-128X4
 * @ctx: An AEGIS-128X4 context
 * @dst: The destination buffer.  Can be in-place or out-of-place.  For other
 *	 overlaps the behavior is unspecified.
 * @src: The source plaintext data
 * @len: Number of bytes to encrypt.  The caller must ensure that the total
 *	 number of bytes encrypted stays below 2^61.
 *
 * This can be called only after all associated data has been processed.
 *
 * Context: Any context.
 */
void aegis128x4_encrypt_update(struct aegis128x4_ctx *ctx, u8 *dst,
			       const u8 *src, size_t len);

/**
 * aegis128x4_decrypt_update() - Incrementally decrypt data with AEGIS-128X4
 * @ctx: An AEGIS-128X4 context
 * @dst: The destination buffer.  Can be in-place or out-of-place.  For other
 *	 overlaps the behavior is unspecified.
 * @src: The source ciphertext data (not including auth tag)
 * @len: Number of bytes to decrypt.  The caller must ensure that the total
 *	 number of bytes decrypted stays below 2^61.
 *
 * This can be called only after all associated data has been processed.
 *
 * IMPORTANT: Callers MUST NOT assume that any decrypted or associated data is
 * authentic until the authentication tag has been verified.
 *
 * Context: Any context.
 */
void aegis128x4_decrypt_update(struct aegis128x4_ctx *ctx, u8 *dst,
			       const u8 *src, size_t len);

/**
 * aegis128x4_encrypt_final() - Finish encrypting a message with AEGIS-128X4
 * @ctx: An AEGIS-128X4 context
 * @authtag: The output authentication tag.  Length is the authtag_len that was
 *	     passed to aegis128x4_preparekey().
 *
 * This also zeroizes @ctx, so the caller doesn't need to do it.
 *
 * Context: Any context.
 */
void aegis128x4_encrypt_final(struct aegis128x4_ctx *ctx, u8 *authtag);

/**
 * aegis128x4_decrypt_final() - Finish decrypting a message with AEGIS-128X4
 * @ctx: An AEGIS-128X4 context
 * @authtag: The stored authentication tag.  Length is the authtag_len that was
 *	     passed to aegis128x4_preparekey().
 *
 * This also zeroizes @ctx, so the caller doesn't need to do it.
 *
 * Context: Any context.
 * Return:
 * * 0 on success.  This is the only case where any decrypted or associated data
 *   can be used.
 * * -EBADMSG if the message is inauthentic
 */
int __must_check aegis128x4_decrypt_final(struct aegis128x4_ctx *ctx,
					  const u8 *authtag);

#endif /* _CRYPTO_AEGIS128X4_H */
