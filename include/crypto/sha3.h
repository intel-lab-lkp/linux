/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common values for SHA-3 algorithms
 *
 * See also Documentation/crypto/sha3.rst
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

/* SHAKE128 and SHAKE256 actually have variable output size, but this is
 * used to calculate the block size/rate analogously to the above..
 */
#define SHAKE128_DEFAULT_SIZE	(128 / 8)
#define SHAKE128_BLOCK_SIZE	(200 - 2 * SHAKE128_DEFAULT_SIZE)
#define SHAKE256_DEFAULT_SIZE	(256 / 8)
#define SHAKE256_BLOCK_SIZE	(200 - 2 * SHAKE256_DEFAULT_SIZE)

#define SHA3_STATE_SIZE		200

struct shash_desc;

struct sha3_state {
	u64		st[SHA3_STATE_SIZE / 8];
};

/*
 * The SHA3 context structure and state buffer.
 *
 * To avoid the need to byteswap when adding input and extracting output from
 * the state array, the state array is kept in little-endian order most of the
 * time, but is byteswapped to host-endian in order to perform the Keccak
 * function and then byteswapped back again after.  On a LE machine, the
 * byteswap step is a no-op.
 */
struct sha3_ctx {
	struct sha3_state	state;
	u8			block_size;	/* Block size in bytes */
	u8			padding;	/* Padding byte */
	u8			absorb_offset;	/* Next state byte to absorb into */
	u8			squeeze_offset;	/* Next state byte to extract */
	bool			end_marked;	/* T if end marker inserted */
};

int crypto_sha3_init(struct shash_desc *desc);

/**
 * sha3_clear() - Explicitly clear the entire context
 * @ctx: the context to clear
 *
 * Explicitly clear the entire context, including the type parameters; after
 * this, the context must be fully initialized again.
 *
 * Context: Any context.
 */
static inline void sha3_clear(struct sha3_ctx *ctx)
{
	memzero_explicit(ctx, sizeof(*ctx));
}

void sha3_update(struct sha3_ctx *ctx, const u8 *data, size_t len);
void sha3_squeeze(struct sha3_ctx *ctx, u8 *out, size_t out_len);

/*
 * Context wrapper for SHA3-224.
 */
struct sha3_224_ctx {
	struct sha3_ctx ctx;
};

/**
 * sha3_224_init() - Set a SHA3 context for SHA3-224
 * @ctx: the context to initialize
 *
 * Initialize a SHA3 context for the production of a SHA3-224 digest of a
 * message.
 *
 * Context: Any context.
 */
static inline void sha3_224_init(struct sha3_224_ctx *ctx)
{
	*ctx = (struct sha3_224_ctx){
		.ctx.block_size	= SHA3_224_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_224_update() - Update a SHA3-224 hash with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to add data to the hash, performing
 * the "keccak sponge absorbing" phase.
 *
 * Context: Any context.
 */
static inline void sha3_224_update(struct sha3_224_ctx *ctx, const u8 *data, size_t len)
{
	return sha3_update(&ctx->ctx, data, len);
}

/**
 * sha3_224_final() - Finalise a SHA3-224 hash and extract the digest
 * @ctx: The context to finalise; must have been initialized
 * @out: Where to write the resulting message digest
 *
 * Finish the computation of a SHA3-224 hash and perform the "Keccak sponge
 * squeezing" phase.  The digest is written to @out buffer and the context will
 * be completely zeroed out.
 *
 * Context: Any context.
 */
static inline void sha3_224_final(struct sha3_224_ctx *ctx, u8 out[SHA3_224_DIGEST_SIZE])
{
	sha3_squeeze(&ctx->ctx, out, SHA3_224_DIGEST_SIZE);
	sha3_clear(&ctx->ctx);
}

/*
 * Context wrapper for SHA3-256.
 */
struct sha3_256_ctx {
	struct sha3_ctx ctx;
};

/**
 * sha3_256_init() - Set a SHA3 context for SHA3-256
 * @ctx: the context to initialize
 *
 * Initialize a SHA3 context for the production of a SHA3-256 digest of a
 * message.
 *
 * Context: Any context.
 */
static inline void sha3_256_init(struct sha3_256_ctx *ctx)
{
	*ctx = (struct sha3_256_ctx){
		.ctx.block_size	= SHA3_256_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_256_update() - Update a SHA3-256 hash with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to add data to the hash, performing
 * the "keccak sponge absorbing" phase.
 *
 * Context: Any context.
 */
static inline void sha3_256_update(struct sha3_256_ctx *ctx, const u8 *data, size_t len)
{
	return sha3_update(&ctx->ctx, data, len);
}

/**
 * sha3_256_final() - Finalise a SHA3-256 hash and extract the digest
 * @ctx: The context to finalise; must have been initialized
 * @out: Where to write the resulting message digest
 *
 * Finish the computation of a SHA3-256 hash and perform the "Keccak sponge
 * squeezing" phase.  The digest is written to @out buffer and the context will
 * be completely zeroed out.
 *
 * Context: Any context.
 */
static inline void sha3_256_final(struct sha3_256_ctx *ctx, u8 out[SHA3_256_DIGEST_SIZE])
{
	sha3_squeeze(&ctx->ctx, out, SHA3_256_DIGEST_SIZE);
	sha3_clear(&ctx->ctx);
}

/*
 * Context wrapper for SHA3-384.
 */
struct sha3_384_ctx {
	struct sha3_ctx ctx;
};

/**
 * sha3_384_init() - Set a SHA3 context for SHA3-384
 * @ctx: the context to initialize
 *
 * Initialize a SHA3 context for the production of a SHA3-384 digest of a
 * message.
 *
 * Context: Any context.
 */
static inline void sha3_384_init(struct sha3_384_ctx *ctx)
{
	*ctx = (struct sha3_384_ctx){
		.ctx.block_size	= SHA3_384_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_384_update() - Update a SHA3-384 hash with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to add data to the hash, performing
 * the "keccak sponge absorbing" phase.
 *
 * Context: Any context.
 */
static inline void sha3_384_update(struct sha3_384_ctx *ctx, const u8 *data, size_t len)
{
	return sha3_update(&ctx->ctx, data, len);
}

/**
 * sha3_384_final() - Finalise a SHA3-384 hash and extract the digest
 * @ctx: The context to finalise; must have been initialized
 * @out: Where to write the resulting message digest
 *
 * Finish the computation of a SHA3-384 hash and perform the "Keccak sponge
 * squeezing" phase.  The digest is written to @out buffer and the context will
 * be completely zeroed out.
 *
 * Context: Any context.
 */
static inline void sha3_384_final(struct sha3_384_ctx *ctx, u8 out[SHA3_384_DIGEST_SIZE])
{
	sha3_squeeze(&ctx->ctx, out, SHA3_384_DIGEST_SIZE);
	sha3_clear(&ctx->ctx);
}

/*
 * Context wrapper for SHA3-512.
 */
struct sha3_512_ctx {
	struct sha3_ctx ctx;
};

/**
 * sha3_512_init() - Set a SHA3 context for SHA3-512
 * @ctx: the context to initialize
 *
 * Initialize a SHA3 context for the production of a SHA3-512 digest of a
 * message.
 *
 * Context: Any context.
 */
static inline void sha3_512_init(struct sha3_512_ctx *ctx)
{
	*ctx = (struct sha3_512_ctx){
		.ctx.block_size	= SHA3_512_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_512_update() - Update a SHA3-512 hash with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to add data to the hash, performing
 * the "keccak sponge absorbing" phase.
 *
 * Context: Any context.
 */
static inline void sha3_512_update(struct sha3_512_ctx *ctx, const u8 *data, size_t len)
{
	return sha3_update(&ctx->ctx, data, len);
}

/**
 * sha3_512_final() - Finalise a SHA3-512 hash and extract the digest
 * @ctx: The context to finalise; must have been initialized
 * @out: Where to write the resulting message digest
 *
 * Finish the computation of a SHA3-512 hash and perform the "Keccak sponge
 * squeezing" phase.  The digest is written to @out buffer and the context will
 * be completely zeroed out.
 *
 * Context: Any context.
 */
static inline void sha3_512_final(struct sha3_512_ctx *ctx, u8 out[SHA3_512_DIGEST_SIZE])
{
	sha3_squeeze(&ctx->ctx, out, SHA3_512_DIGEST_SIZE);
	sha3_clear(&ctx->ctx);
}

/*
 * Context wrapper for SHAKE128.
 */
struct shake128_ctx {
	struct sha3_ctx ctx;
};

/**
 * shake128_init() - Set a SHA3 context for SHAKE128
 * @ctx: The context to initialize
 *
 * Initialize a SHA3 context for the production of SHAKE128 output generation
 * from a message.  The sha3_squeeze() function can be used to extract an
 * arbitrary amount of data from the context.
 *
 * Context: Any context.
 */
static inline void shake128_init(struct shake128_ctx *ctx)
{
	*ctx = (struct shake128_ctx){
		.ctx.block_size	= SHAKE128_BLOCK_SIZE,
		.ctx.padding	= 0x1f,
	};
}

/**
 * shake128_update() - Update a SHAKE128 hash with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to add data to the XOF state,
 * performing the "keccak sponge absorbing" phase.
 *
 * Context: Any context.
 */
static inline void shake128_update(struct shake128_ctx *ctx, const u8 *data, size_t len)
{
	return sha3_update(&ctx->ctx, data, len);
}

/**
 * shake128_squeeze() - Finalize a SHAKE128 digest of any type and extract the digest
 * @ctx: the context to finalize; must have been initialized
 * @out: Where to write the resulting message digest
 * @out_size: The amount of digest to extract to @out in bytes
 *
 * Finish the computation of a SHAKE128 XOF and perform the "Keccak sponge
 * squeezing" phase.  @out_size amount of digest is written to @out buffer.
 *
 * This may be called multiple times to extract continuations of the digest.
 * Note that, a number of consecutive squeezes laid end-to-end will yield the
 * same output as one big squeeze generating the same total amount of output.
 *
 * Context: Any context.
 */
static inline void shake128_squeeze(struct shake128_ctx *ctx, u8 *out, size_t out_size)
{
	return sha3_squeeze(&ctx->ctx, out, out_size);
}

/**
 * shake128_clear() - Explicitly clear the entire SHAKE128 context
 * @ctx: the context to clear
 *
 * Explicitly clear the entire context; after this, the context must be
 * initialized again.
 *
 * Context: Any context.
 */
static inline void shake128_clear(struct shake128_ctx *ctx)
{
	sha3_clear(&ctx->ctx);
}

/*
 * Context wrapper for SHAKE256.
 */
struct shake256_ctx {
	struct sha3_ctx ctx;
};

/**
 * shake256_init() - Set a SHA3 context for SHAKE256
 * @ctx: The context to initialize
 *
 * Initialize a SHA3 context for the production of SHAKE256 output generation
 * from a message.  The sha3_squeeze() function can be used to extract an
 * arbitrary amount of data from the context.
 *
 * Context: Any context.
 */
static inline void shake256_init(struct shake256_ctx *ctx)
{
	*ctx = (struct shake256_ctx){
		.ctx.block_size	= SHAKE256_BLOCK_SIZE,
		.ctx.padding	= 0x1f,
	};
}

/**
 * shake256_update() - Update a SHAKE256 hash with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to add data to the XOF state,
 * performing the "keccak sponge absorbing" phase.
 *
 * Context: Any context.
 */
static inline void shake256_update(struct shake256_ctx *ctx, const u8 *data, size_t len)
{
	return sha3_update(&ctx->ctx, data, len);
}

/**
 * shake256_squeeze() - Finalize a SHAKE256 digest of any type and extract the digest
 * @ctx: the context to finalize; must have been initialized
 * @out: Where to write the resulting message digest
 * @out_size: The amount of digest to extract to @out in bytes
 *
 * Finish the computation of a SHAKE256 XOF and perform the "Keccak sponge
 * squeezing" phase.  @out_size amount of digest is written to @out buffer.
 *
 * This may be called multiple times to extract continuations of the digest.
 * Note that, a number of consecutive squeezes laid end-to-end will yield the
 * same output as one big squeeze generating the same total amount of output.
 *
 * Context: Any context.
 */
static inline void shake256_squeeze(struct shake256_ctx *ctx, u8 *out, size_t out_size)
{
	return sha3_squeeze(&ctx->ctx, out, out_size);
}

/**
 * shake256_clear() - Explicitly clear the entire SHAKE256 context
 * @ctx: the context to clear
 *
 * Explicitly clear the entire context; after this, the context must be
 * initialized again.
 *
 * Context: Any context.
 */
static inline void shake256_clear(struct shake256_ctx *ctx)
{
	sha3_clear(&ctx->ctx);
}

void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE]);
void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE]);
void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE]);
void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE]);
void shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len);
void shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len);

#endif /* __CRYPTO_SHA3_H__ */
