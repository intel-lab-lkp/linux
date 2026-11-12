// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * AEGIS-128X4 authenticated encryption and decryption (RFC 10032)
 *
 * AEGIS-128X4 runs four AEGIS-128L instances in parallel, which differ only
 * by the context separator mixed in during initialization, and combines their
 * authentication tags.  It is designed for CPUs that can apply the AES round
 * function to four blocks at once.
 *
 * Copyright 2026 Antonio Quartulli <antonio@mandelbit.com>
 */

#include <crypto/aegis128x4.h>
#include <crypto/aes.h>
#include <crypto/utils.h>
#include <linux/cache.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#define LANES		AEGIS128X4_LANES
#define BLOCK_SIZE	AEGIS128X4_BLOCK_SIZE
#define HALF_SIZE	(BLOCK_SIZE / 2)

/* Maximum associated data and en/decrypted data lengths in bytes */
#define AEGIS128X4_MAX_LEN ((1ULL << 61) - 1)

static const u8 aegis_c0[16] = {
	0x00, 0x01, 0x01, 0x02, 0x03, 0x05, 0x08, 0x0d,
	0x15, 0x22, 0x37, 0x59, 0x90, 0xe9, 0x79, 0x62,
};

static const u8 aegis_c1[16] = {
	0xdb, 0x3d, 0x18, 0x55, 0x6d, 0xc2, 0x2f, 0xf1,
	0x20, 0x11, 0x31, 0x42, 0x73, 0xb5, 0x28, 0xdd,
};

#ifdef CONFIG_CRYPTO_LIB_AEGIS128X4_ARCH
#include "aegis128x4.h" /* $(SRCARCH)/aegis128x4.h */
#else
static bool aegis128x4_init_arch(union aegis128x4_state *st,
				 const u8 key[AEGIS128X4_KEY_SIZE],
				 const u8 nonce[AEGIS128X4_NONCE_SIZE])
{
	return false;
}

static bool aegis128x4_absorb_arch(union aegis128x4_state *st, const u8 *src,
				   size_t nblocks)
{
	return false;
}

static bool aegis128x4_enc_arch(union aegis128x4_state *st, u8 *dst,
				const u8 *src, size_t nblocks)
{
	return false;
}

static bool aegis128x4_dec_arch(union aegis128x4_state *st, u8 *dst,
				const u8 *src, size_t nblocks)
{
	return false;
}

static bool aegis128x4_final_arch(union aegis128x4_state *st, u8 *authtag,
				  u64 ad_len, u64 data_len, size_t authtag_len)
{
	return false;
}

static bool aegis128x4_crypt_arch(u8 *dst, const u8 *src, size_t data_len,
				  u8 *authtag, const u8 *ad, size_t ad_len,
				  const u8 nonce[AEGIS128X4_NONCE_SIZE],
				  const struct aegis128x4_key *key, bool enc)
{
	return false;
}
#endif

/* Prefetch the AES table to mitigate cache-timing attacks. */
static __always_inline void aegis128x4_prefetch_tab(void)
{
	for (size_t i = 0; i < sizeof(aes_enc_tab); i += L1_CACHE_BYTES)
		*(volatile const u8 *)((const u8 *)aes_enc_tab + i);
	barrier();
}

/*
 * dst ^= AESRound(src, 0), i.e. dst = AESRound(src, dst), on the 16-byte block
 * of each lane.  @dst and @src must be different state words.
 */
static __always_inline void aegis128x4_aesround(__le32 dst[4 * LANES],
						const __le32 src[4 * LANES])
{
	const u32 *t = aes_enc_tab;

	for (int i = 0; i < LANES; i++) {
		const u8 *s = (const u8 *)&src[4 * i];
		__le32 *d = &dst[4 * i];

		d[0] ^= cpu_to_le32(t[s[0]] ^ rol32(t[s[5]], 8) ^
				    rol32(t[s[10]], 16) ^ rol32(t[s[15]], 24));
		d[1] ^= cpu_to_le32(t[s[4]] ^ rol32(t[s[9]], 8) ^
				    rol32(t[s[14]], 16) ^ rol32(t[s[3]], 24));
		d[2] ^= cpu_to_le32(t[s[8]] ^ rol32(t[s[13]], 8) ^
				    rol32(t[s[2]], 16) ^ rol32(t[s[7]], 24));
		d[3] ^= cpu_to_le32(t[s[12]] ^ rol32(t[s[1]], 8) ^
				    rol32(t[s[6]], 16) ^ rol32(t[s[11]], 24));
	}
}

/* The Update function: absorb @m0 and @m1, each HALF_SIZE bytes. */
static void aegis128x4_update(union aegis128x4_state *st, const u8 *m0,
			      const u8 *m1)
{
	__le32 s7[4 * LANES];

	/*
	 * Each new word j is AESRound(S[j - 1], S[j]), with the message
	 * XOR'ed into words 0 and 4.  Compute them from the last one down, so
	 * that S[j - 1] is still the old value, except for word 0, which needs
	 * the old word 7.
	 */
	memcpy(s7, st->w32[7], sizeof(s7));
	for (int j = 7; j > 0; j--)
		aegis128x4_aesround(st->w32[j], st->w32[j - 1]);
	aegis128x4_aesround(st->w32[0], s7);
	crypto_xor(st->b[0], m0, HALF_SIZE);
	crypto_xor(st->b[4], m1, HALF_SIZE);
	memzero_explicit(s7, sizeof(s7));
}

/* Compute the keystream of the next block from the current state. */
static void aegis128x4_keystream(const union aegis128x4_state *st,
				 u8 z[BLOCK_SIZE])
{
	for (int i = 0; i < 2 * LANES; i++) {
		u64 z0 = st->w64[1][i] ^ st->w64[6][i] ^
			 (st->w64[2][i] & st->w64[3][i]);
		u64 z1 = st->w64[2][i] ^ st->w64[5][i] ^
			 (st->w64[6][i] & st->w64[7][i]);

		memcpy(&z[8 * i], &z0, 8);
		memcpy(&z[HALF_SIZE + 8 * i], &z1, 8);
	}
}

static void aegis128x4_init_generic(union aegis128x4_state *st,
				    const u8 key[AEGIS128X4_KEY_SIZE],
				    const u8 nonce[AEGIS128X4_NONCE_SIZE])
{
	u8 key_v[HALF_SIZE], nonce_v[HALF_SIZE];

	for (int i = 0; i < LANES; i++) {
		u8 *k = &key_v[16 * i], *n = &nonce_v[16 * i];

		memcpy(k, key, 16);
		memcpy(n, nonce, 16);
		crypto_xor_cpy(&st->b[0][16 * i], k, n, 16);
		memcpy(&st->b[1][16 * i], aegis_c1, 16);
		memcpy(&st->b[2][16 * i], aegis_c0, 16);
		memcpy(&st->b[3][16 * i], aegis_c1, 16);
		crypto_xor_cpy(&st->b[4][16 * i], k, n, 16);
		crypto_xor_cpy(&st->b[5][16 * i], k, aegis_c0, 16);
		crypto_xor_cpy(&st->b[6][16 * i], k, aegis_c1, 16);
		crypto_xor_cpy(&st->b[7][16 * i], k, aegis_c0, 16);
	}

	for (int r = 0; r < 10; r++) {
		/* ctx[i]: the lane index, then the highest lane index */
		for (int i = 0; i < LANES; i++) {
			st->b[3][16 * i] ^= i;
			st->b[3][16 * i + 1] ^= LANES - 1;
			st->b[7][16 * i] ^= i;
			st->b[7][16 * i + 1] ^= LANES - 1;
		}
		aegis128x4_update(st, nonce_v, key_v);
	}
	memzero_explicit(key_v, sizeof(key_v));
}

static void aegis128x4_init_state(union aegis128x4_state *st,
				  const u8 key[AEGIS128X4_KEY_SIZE],
				  const u8 nonce[AEGIS128X4_NONCE_SIZE])
{
	if (!aegis128x4_init_arch(st, key, nonce)) {
		aegis128x4_prefetch_tab();
		aegis128x4_init_generic(st, key, nonce);
	}
}

/* Absorb @nblocks full blocks. */
static void aegis128x4_absorb(union aegis128x4_state *st, const u8 *src,
			      size_t nblocks)
{
	if (!nblocks || aegis128x4_absorb_arch(st, src, nblocks))
		return;

	aegis128x4_prefetch_tab();
	for (; nblocks; nblocks--, src += BLOCK_SIZE)
		aegis128x4_update(st, src, src + HALF_SIZE);
}

/* En/decrypt @nblocks full blocks. */
static void aegis128x4_crypt_blocks(union aegis128x4_state *st, u8 *dst,
				    const u8 *src, size_t nblocks, bool enc)
{
	u8 z[BLOCK_SIZE], p[BLOCK_SIZE];

	if (!nblocks || (enc ? aegis128x4_enc_arch(st, dst, src, nblocks) :
			       aegis128x4_dec_arch(st, dst, src, nblocks)))
		return;

	aegis128x4_prefetch_tab();
	for (; nblocks; nblocks--, src += BLOCK_SIZE, dst += BLOCK_SIZE) {
		aegis128x4_keystream(st, z);
		/*
		 * The state absorbs the plaintext, so read it before writing
		 * @dst, which may be the same buffer.
		 */
		if (enc) {
			aegis128x4_update(st, src, src + HALF_SIZE);
			crypto_xor_cpy(dst, src, z, BLOCK_SIZE);
		} else {
			crypto_xor_cpy(p, src, z, BLOCK_SIZE);
			memcpy(dst, p, BLOCK_SIZE);
			aegis128x4_update(st, p, p + HALF_SIZE);
		}
	}
	memzero_explicit(z, sizeof(z));
	memzero_explicit(p, sizeof(p));
}

static void aegis128x4_final_generic(union aegis128x4_state *st, u8 *authtag,
				     u64 ad_len, u64 data_len,
				     size_t authtag_len)
{
	u8 t[HALF_SIZE], tag[AEGIS128X4_MAX_AUTHTAG_SIZE] = {};

	for (int i = 0; i < LANES; i++) {
		put_unaligned_le64(ad_len * 8, &t[16 * i]);
		put_unaligned_le64(data_len * 8, &t[16 * i + 8]);
	}
	crypto_xor(t, st->b[2], HALF_SIZE);

	for (int r = 0; r < 7; r++)
		aegis128x4_update(st, t, t);

	for (int i = 0; i < LANES; i++) {
		if (authtag_len == 16) {
			for (int j = 0; j < 7; j++)
				crypto_xor(tag, &st->b[j][16 * i], 16);
		} else {
			for (int j = 0; j < 4; j++)
				crypto_xor(tag, &st->b[j][16 * i], 16);
			for (int j = 4; j < 8; j++)
				crypto_xor(&tag[16], &st->b[j][16 * i], 16);
		}
	}
	memcpy(authtag, tag, authtag_len);
	memzero_explicit(t, sizeof(t));
	memzero_explicit(tag, sizeof(tag));
}

static void aegis128x4_final_state(union aegis128x4_state *st, u8 *authtag,
				   u64 ad_len, u64 data_len,
				   size_t authtag_len)
{
	if (!aegis128x4_final_arch(st, authtag, ad_len, data_len,
				   authtag_len)) {
		aegis128x4_prefetch_tab();
		aegis128x4_final_generic(st, authtag, ad_len, data_len,
					 authtag_len);
	}
}

int aegis128x4_preparekey(struct aegis128x4_key *key, const u8 *in_key,
			  size_t key_len, size_t authtag_len)
{
	if (key_len != AEGIS128X4_KEY_SIZE ||
	    (authtag_len != 16 && authtag_len != 32))
		return -EINVAL;
	memcpy(key->key, in_key, AEGIS128X4_KEY_SIZE);
	key->authtag_len = authtag_len;
	return 0;
}
EXPORT_SYMBOL_GPL(aegis128x4_preparekey);

void aegis128x4_init(struct aegis128x4_ctx *ctx,
		     const u8 nonce[AEGIS128X4_NONCE_SIZE],
		     const struct aegis128x4_key *key)
{
	aegis128x4_init_state(&ctx->state, key->key, nonce);
	ctx->key = key;
	ctx->ad_len = 0;
	ctx->data_len = 0;
	ctx->data = false;
}
EXPORT_SYMBOL_GPL(aegis128x4_init);

void aegis128x4_auth_update(struct aegis128x4_ctx *ctx, const u8 *ad,
			    size_t len)
{
	size_t pos = ctx->ad_len % BLOCK_SIZE;

	WARN_ON_ONCE(ctx->data);
	ctx->ad_len += len;
	if (pos) {
		size_t n = min(len, BLOCK_SIZE - pos);

		memcpy(&ctx->buf[pos], ad, n);
		if (pos + n < BLOCK_SIZE)
			return;
		aegis128x4_absorb(&ctx->state, ctx->buf, 1);
		ad += n;
		len -= n;
	}
	aegis128x4_absorb(&ctx->state, ad, len / BLOCK_SIZE);
	memcpy(ctx->buf, ad + round_down(len, BLOCK_SIZE), len % BLOCK_SIZE);
}
EXPORT_SYMBOL_GPL(aegis128x4_auth_update);

/* Absorb the zero-padded incomplete block of @len bytes, if any. */
static void aegis128x4_flush(struct aegis128x4_ctx *ctx, size_t len)
{
	size_t pos = len % BLOCK_SIZE;

	if (pos) {
		memset(&ctx->buf[pos], 0, BLOCK_SIZE - pos);
		aegis128x4_absorb(&ctx->state, ctx->buf, 1);
	}
}

static void aegis128x4_crypt_update(struct aegis128x4_ctx *ctx, u8 *dst,
				    const u8 *src, size_t len, bool enc)
{
	size_t pos = ctx->data_len % BLOCK_SIZE;
	size_t nblocks, tail;
	u8 z[BLOCK_SIZE];

	if (!ctx->data) {
		aegis128x4_flush(ctx, ctx->ad_len);
		ctx->data = true;
	}
	ctx->data_len += len;

	/*
	 * The incomplete block keeps its plaintext in ctx->buf.  Its keystream
	 * depends only on the state, which changes once it's complete.
	 */
	if (pos) {
		size_t n = min(len, BLOCK_SIZE - pos);

		aegis128x4_keystream(&ctx->state, z);
		if (enc) {
			memcpy(&ctx->buf[pos], src, n);
			crypto_xor_cpy(dst, &ctx->buf[pos], &z[pos], n);
		} else {
			crypto_xor_cpy(&ctx->buf[pos], src, &z[pos], n);
			memcpy(dst, &ctx->buf[pos], n);
		}
		if (pos + n < BLOCK_SIZE)
			goto out;
		aegis128x4_absorb(&ctx->state, ctx->buf, 1);
		src += n;
		dst += n;
		len -= n;
	}

	nblocks = len / BLOCK_SIZE;
	aegis128x4_crypt_blocks(&ctx->state, dst, src, nblocks, enc);
	src += nblocks * BLOCK_SIZE;
	dst += nblocks * BLOCK_SIZE;

	tail = len % BLOCK_SIZE;
	if (tail) {
		aegis128x4_keystream(&ctx->state, z);
		if (enc) {
			memcpy(ctx->buf, src, tail);
			crypto_xor_cpy(dst, ctx->buf, z, tail);
		} else {
			crypto_xor_cpy(ctx->buf, src, z, tail);
			memcpy(dst, ctx->buf, tail);
		}
	}
out:
	memzero_explicit(z, sizeof(z));
}

void aegis128x4_encrypt_update(struct aegis128x4_ctx *ctx, u8 *dst,
			       const u8 *src, size_t len)
{
	aegis128x4_crypt_update(ctx, dst, src, len, true);
}
EXPORT_SYMBOL_GPL(aegis128x4_encrypt_update);

void aegis128x4_decrypt_update(struct aegis128x4_ctx *ctx, u8 *dst,
			       const u8 *src, size_t len)
{
	aegis128x4_crypt_update(ctx, dst, src, len, false);
}
EXPORT_SYMBOL_GPL(aegis128x4_decrypt_update);

static void aegis128x4_compute_tag(struct aegis128x4_ctx *ctx, u8 *authtag)
{
	if (!ctx->data)
		aegis128x4_flush(ctx, ctx->ad_len);
	else
		aegis128x4_flush(ctx, ctx->data_len);
	aegis128x4_final_state(&ctx->state, authtag, ctx->ad_len,
			       ctx->data_len, ctx->key->authtag_len);
}

void aegis128x4_encrypt_final(struct aegis128x4_ctx *ctx, u8 *authtag)
{
	WARN_ON_ONCE(ctx->ad_len > AEGIS128X4_MAX_LEN);
	WARN_ON_ONCE(ctx->data_len > AEGIS128X4_MAX_LEN);

	aegis128x4_compute_tag(ctx, authtag);
	aegis128x4_zeroize_ctx(ctx);
}
EXPORT_SYMBOL_GPL(aegis128x4_encrypt_final);

int aegis128x4_decrypt_final(struct aegis128x4_ctx *ctx, const u8 *authtag)
{
	size_t authtag_len = ctx->key->authtag_len;
	u8 expected[AEGIS128X4_MAX_AUTHTAG_SIZE];
	int err;

	if (WARN_ON_ONCE(ctx->ad_len > AEGIS128X4_MAX_LEN) ||
	    WARN_ON_ONCE(ctx->data_len > AEGIS128X4_MAX_LEN)) {
		aegis128x4_zeroize_ctx(ctx);
		return -EBADMSG;
	}

	aegis128x4_compute_tag(ctx, expected);
	err = crypto_memneq(expected, authtag, authtag_len) ? -EBADMSG : 0;
	aegis128x4_zeroize_ctx(ctx);
	memzero_explicit(expected, sizeof(expected));
	return err;
}
EXPORT_SYMBOL_GPL(aegis128x4_decrypt_final);

void aegis128x4_encrypt(u8 *dst, const u8 *src, size_t data_len, u8 *authtag,
			const u8 *ad, size_t ad_len,
			const u8 nonce[AEGIS128X4_NONCE_SIZE],
			const struct aegis128x4_key *key)
{
	struct aegis128x4_ctx ctx;

	if (aegis128x4_crypt_arch(dst, src, data_len, authtag, ad, ad_len,
				  nonce, key, true))
		return;

	aegis128x4_init(&ctx, nonce, key);
	aegis128x4_auth_update(&ctx, ad, ad_len);
	aegis128x4_encrypt_update(&ctx, dst, src, data_len);
	aegis128x4_encrypt_final(&ctx, authtag);
}
EXPORT_SYMBOL_GPL(aegis128x4_encrypt);

int aegis128x4_decrypt(u8 *dst, const u8 *src, size_t data_len,
		       const u8 *authtag, const u8 *ad, size_t ad_len,
		       const u8 nonce[AEGIS128X4_NONCE_SIZE],
		       const struct aegis128x4_key *key)
{
	struct aegis128x4_ctx ctx;
	u8 expected[AEGIS128X4_MAX_AUTHTAG_SIZE];
	int err;

	if (aegis128x4_crypt_arch(dst, src, data_len, expected, ad, ad_len,
				  nonce, key, false)) {
		err = crypto_memneq(expected, authtag, key->authtag_len) ?
			      -EBADMSG : 0;
		memzero_explicit(expected, sizeof(expected));
	} else {
		aegis128x4_init(&ctx, nonce, key);
		aegis128x4_auth_update(&ctx, ad, ad_len);
		aegis128x4_decrypt_update(&ctx, dst, src, data_len);
		err = aegis128x4_decrypt_final(&ctx, authtag);
	}
	if (unlikely(err) && data_len) {
		/*
		 * Clear the inauthentic decrypted data so that callers won't
		 * receive it even if they fail to correctly handle errors.
		 */
		memzero_explicit(dst, data_len);
	}
	return err;
}
EXPORT_SYMBOL_GPL(aegis128x4_decrypt);

#ifdef aegis128x4_mod_init_arch
static int __init aegis128x4_mod_init(void)
{
	aegis128x4_mod_init_arch();
	return 0;
}
subsys_initcall(aegis128x4_mod_init);

static void __exit aegis128x4_mod_exit(void)
{
}
module_exit(aegis128x4_mod_exit);
#endif

MODULE_DESCRIPTION("AEGIS-128X4 authenticated encryption algorithm");
MODULE_LICENSE("GPL");
