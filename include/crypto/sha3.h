/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common values for SHA-3 algorithms
 */
#ifndef __CRYPTO_SHA3_H__
#define __CRYPTO_SHA3_H__

#include <linux/types.h>

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
	__le64			buf[SHA3_224_BLOCK_SIZE / 8];
	unsigned short		block_size;
	unsigned short		digest_size;
	unsigned short		partial;
	u8			padding;
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
 * @digest_size: The size of the digest desired
 */
static inline void shake128_init(struct sha3_ctx *ctx, unsigned int digest_size)
{
	sha3_init(ctx);
	ctx->block_size  = SHAKE128_BLOCK_SIZE;
	ctx->digest_size = digest_size;
	ctx->padding	 = 0x1f;
}

/**
 * shake256_init() - Initialize a SHAKE256 context for a new message
 * @ctx: the context to initialize
 * @digest_size: The size of the digest desired
 */
static inline void shake256_init(struct sha3_ctx *ctx, unsigned int digest_size)
{
	sha3_init(ctx);
	ctx->block_size  = SHA3_256_BLOCK_SIZE;
	ctx->digest_size = digest_size;
	ctx->padding	 = 0x1f;
}

void sha3_update(struct sha3_ctx *ctx, const u8 *data, unsigned int len);
int sha3_final(struct sha3_ctx *ctx, u8 *out);
void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE]);
void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE]);
void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE]);
void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE]);
int shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len);
int shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len);

#endif /* __CRYPTO_SHA3_H__ */
