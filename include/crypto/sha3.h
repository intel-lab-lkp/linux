/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common values for SHA-3 algorithms
 */
#ifndef __CRYPTO_SHA3_H__
#define __CRYPTO_SHA3_H__

#include <linux/types.h>
#include <linux/string.h>

#define SHA3_224_DIGEST_SIZE	(224 / 8)
#define SHA3_224_BLOCK_SIZE	(200 - 2 * SHA3_224_DIGEST_SIZE)
#define SHA3_224_EXPORT_SIZE	SHA3_STATE_SIZE + SHA3_224_BLOCK_SIZE + 1

#define SHA3_256_DIGEST_SIZE	(256 / 8)
#define SHA3_256_BLOCK_SIZE	(200 - 2 * SHA3_256_DIGEST_SIZE)
#define SHA3_256_EXPORT_SIZE	SHA3_STATE_SIZE + SHA3_256_BLOCK_SIZE + 1

#define SHA3_384_DIGEST_SIZE	(384 / 8)
#define SHA3_384_BLOCK_SIZE	(200 - 2 * SHA3_384_DIGEST_SIZE)
#define SHA3_384_EXPORT_SIZE	SHA3_STATE_SIZE + SHA3_384_BLOCK_SIZE + 1

#define SHA3_512_DIGEST_SIZE	(512 / 8)
#define SHA3_512_BLOCK_SIZE	(200 - 2 * SHA3_512_DIGEST_SIZE)
#define SHA3_512_EXPORT_SIZE	SHA3_STATE_SIZE + SHA3_512_BLOCK_SIZE + 1

/* SHAKE128 and SHAKE256 actually have variable digest size, but this
 * is needed for the kunit tests.
 */
#define SHAKE128_DIGEST_SIZE	(128 / 8)
#define SHAKE128_BLOCK_SIZE	(200 - 2 * SHAKE128_DIGEST_SIZE)
#define SHAKE256_DIGEST_SIZE	(256 / 8)
#define SHAKE256_BLOCK_SIZE	(200 - 2 * SHAKE256_DIGEST_SIZE)

#define SHA3_STATE_SIZE		200

struct shash_desc;

struct sha3_state {
	u64		st[SHA3_STATE_SIZE / 8];
};

struct sha3_ctx {
	struct sha3_state	state;
	unsigned short		digest_size;	/* Output digest size in bytes */
	u8			block_size;	/* Block size in bytes */
	u8			partial;	/* Next state byte to absorb into */
	u8			padding;	/* Padding byte */
	u8			squeeze_offset;	/* Next state byte to extract */
	bool			end_marked;	/* T if end marker inserted */
};

int crypto_sha3_init(struct shash_desc *desc);

void sha3_init(struct sha3_ctx *ctx);

/**
 * sha3_224_init() - Initialize a SHA3-224 context for a new message
 * @ctx: the context to initialize
 */
static inline void sha3_224_init(struct sha3_ctx *ctx)
{
	sha3_init(ctx);
	ctx->block_size  = SHA3_224_BLOCK_SIZE;
	ctx->digest_size = SHA3_224_DIGEST_SIZE;
	ctx->padding	 = 0x06;
}

/**
 * sha3_256_init() - Initialize a SHA3-256 context for a new message
 * @ctx: the context to initialize
 */
static inline void sha3_256_init(struct sha3_ctx *ctx)
{
	sha3_init(ctx);
	ctx->block_size  = SHA3_256_BLOCK_SIZE;
	ctx->digest_size = SHA3_256_DIGEST_SIZE;
	ctx->padding	 = 0x06;
}

/**
 * sha3_384_init() - Initialize a SHA3-384 context for a new message
 * @ctx: the context to initialize
 */
static inline void sha3_384_init(struct sha3_ctx *ctx)
{
	sha3_init(ctx);
	ctx->block_size  = SHA3_384_BLOCK_SIZE;
	ctx->digest_size = SHA3_384_DIGEST_SIZE;
	ctx->padding	 = 0x06;
}

/**
 * sha3_512_init() - Initialize a SHA3-512 context for a new message
 * @ctx: the context to initialize
 */
static inline void sha3_512_init(struct sha3_ctx *ctx)
{
	sha3_init(ctx);
	ctx->block_size  = SHA3_512_BLOCK_SIZE;
	ctx->digest_size = SHA3_512_DIGEST_SIZE;
	ctx->padding	 = 0x06;
}

/**
 * shake128_init() - Initialize a SHAKE128 context for a new message
 * @ctx: The context to initialize
 */
static inline void shake128_init(struct sha3_ctx *ctx)
{
	sha3_init(ctx);
	ctx->block_size  = SHAKE128_BLOCK_SIZE;
	ctx->digest_size = SHAKE128_DIGEST_SIZE;
	ctx->padding	 = 0x1f;
}

/**
 * shake256_init() - Initialize a SHAKE256 context for a new message
 * @ctx: the context to initialize
 */
static inline void shake256_init(struct sha3_ctx *ctx)
{
	sha3_init(ctx);
	ctx->block_size  = SHAKE256_BLOCK_SIZE;
	ctx->digest_size = SHAKE256_DIGEST_SIZE;
	ctx->padding	 = 0x1f;
}

/**
 * sha3_set_digestsize() - Change the digest size for a SHAKE* hash
 * @ctx: the context to modify
 * @digest_size: The new size of the digest
 */
static inline void sha3_set_digestsize(struct sha3_ctx *ctx, unsigned int digest_size)
{
	ctx->digest_size = digest_size;
}

/**
 * sha3_reinit() - Explicitly clear the hash working state
 * @ctx: the context to clear
 *
 * Explicitly clear the hash state and reset the tracking members, but leave
 * the hash property members unaffected.
 */
static inline void sha3_reinit(struct sha3_ctx *ctx)
{
	memzero_explicit(&ctx->state, sizeof(ctx->state));
	ctx->partial = 0;
	ctx->squeeze_offset = 0;
	ctx->end_marked = false;
}

/**
 * sha3_clear() - Explicitly clear the entire context
 * @ctx: the context to clear
 */
static inline void sha3_clear(struct sha3_ctx *ctx)
{
	memzero_explicit(ctx, sizeof(*ctx));
}

void sha3_update(struct sha3_ctx *ctx, const u8 *data, unsigned int len);
void sha3_squeeze(struct sha3_ctx *ctx, u8 *out, size_t out_len);
void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE]);
void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE]);
void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE]);
void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE]);
void shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len);
void shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len);

/**
 * sha3_squeeze() - Finalize a SHA3 digest of any type and extract the digest
 * @ctx: The context to finalize; must have been initialized
 * @out: Where to write the resulting message digest
 * @out_size: The amount of digest to extract to @out
 *
 * Finish the computation of a SHA3 message digest of any type and perform the
 * "Keccak sponge squeezing" phase.  The digest is written to @out buffer and
 * the size of the digest is returned.  The amount of digest specified in @ctx
 * will be extracted.
 *
 * Upon returning, the context will be zeroed out.
 */
static inline unsigned int sha3_final(struct sha3_ctx *ctx, u8 *out)
{
	unsigned int out_size = ctx->digest_size;

	sha3_squeeze(ctx, out, out_size);
	sha3_clear(ctx);
	return out_size;
}

#endif /* __CRYPTO_SHA3_H__ */
