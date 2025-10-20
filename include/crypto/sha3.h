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

int crypto_sha3_init(struct shash_desc *desc);

struct sha3_state {
	u64		st[SHA3_STATE_SIZE / 8];
};

/**
 * Internal context, shared by the digests (SHA3-*) and the XOFs (SHAKE*).
 *
 * To avoid the need to byteswap when adding input and extracting output from
 * the state array, the state array is kept in little-endian order most of the
 * time, but is byteswapped to host-endian in order to perform the Keccak
 * function and then byteswapped back again after.  On a LE machine, the
 * byteswap step is a no-op.
 */
struct __sha3_ctx {
	struct sha3_state	state;
	u8			block_size;	/* Block size in bytes */
	u8			padding;	/* Padding byte */
	u8			absorb_offset;	/* Next state byte to absorb into */
	u8			squeeze_offset;	/* Next state byte to extract */
	bool			end_marked;	/* T if end marker inserted */
};

void __sha3_update(struct __sha3_ctx *ctx, const u8 *in, size_t in_len);
void __sha3_squeeze(struct __sha3_ctx *ctx, u8 *out, size_t out_len);

/** Context for SHA3-224, SHA3-256, SHA3-384, or SHA3-512 */
struct sha3_ctx {
	struct __sha3_ctx	ctx;
	u8			digest_size;	/* Digest size in bytes */
};

/**
 * Zeroize a sha3_ctx.  This is already called by sha3_final().  Call this
 * explicitly when abandoning a context without calling sha3_final().
 */
static inline void sha3_zeroize_ctx(struct sha3_ctx *ctx)
{
	memzero_explicit(ctx, sizeof(*ctx));
}

/** Context for SHAKE128 or SHAKE256 */
struct shake_ctx {
	struct __sha3_ctx ctx;
};

/** Zeroize a shake_ctx.  Call this after the last squeeze. */
static inline void shake_zeroize_ctx(struct shake_ctx *ctx)
{
	memzero_explicit(ctx, sizeof(*ctx));
}

/**
 * sha3_224_init() - Initialize a context for SHA3-224
 * @ctx: The context to initialize
 *
 * This begins a new SHA3-224 message digest computation.
 *
 * Context: Any context.
 */
static inline void sha3_224_init(struct sha3_ctx *ctx)
{
	*ctx = (struct sha3_ctx){
		.digest_size	= SHA3_224_DIGEST_SIZE,
		.ctx.block_size	= SHA3_224_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_256_init() - Initialize a context for SHA3-256
 * @ctx: The context to initialize
 *
 * This begins a new SHA3-256 message digest computation.
 *
 * Context: Any context.
 */
static inline void sha3_256_init(struct sha3_ctx *ctx)
{
	*ctx = (struct sha3_ctx){
		.digest_size	= SHA3_256_DIGEST_SIZE,
		.ctx.block_size	= SHA3_256_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_384_init() - Initialize a context for SHA3-384
 * @ctx: The context to initialize
 *
 * This begins a new SHA3-384 message digest computation.
 *
 * Context: Any context.
 */
static inline void sha3_384_init(struct sha3_ctx *ctx)
{
	*ctx = (struct sha3_ctx){
		.digest_size	= SHA3_384_DIGEST_SIZE,
		.ctx.block_size	= SHA3_384_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_512_init() - Initialize a context for SHA3-512
 * @ctx: The context to initialize
 *
 * This begins a new SHA3-512 message digest computation.
 *
 * Context: Any context.
 */
static inline void sha3_512_init(struct sha3_ctx *ctx)
{
	*ctx = (struct sha3_ctx){
		.digest_size	= SHA3_512_DIGEST_SIZE,
		.ctx.block_size	= SHA3_512_BLOCK_SIZE,
		.ctx.padding	= 0x06,
	};
}

/**
 * sha3_update() - Update a SHA-3 digest context with input data
 * @ctx: The context to update; must have been initialized
 * @in: The input data
 * @in_len: Length of the input data in bytes
 *
 * This can be called any number of times to add data to a SHA3-224, SHA3-256,
 * SHA3-384, or SHA3-512 digest (depending on which init function was called).
 *
 * Context: Any context.
 */
static inline void sha3_update(struct sha3_ctx *ctx,
			       const u8 *in, size_t in_len)
{
	__sha3_update(&ctx->ctx, in, in_len);
}

/**
 * sha3_final() - Finish computing a SHA-3 message digest
 * @ctx: The context to finalize; must have been initialized
 * @out: (output) The resulting SHA3-224, SHA3-256, SHA3-384, or SHA3-512
 *	 message digest, matching the init function that was called.  Note that
 *	 the size differs for each one; see SHA3_*_DIGEST_SIZE.
 *
 * After finishing, this zeroizes @ctx.  So the caller does not need to do it.
 *
 * Context: Any context.
 */
static inline void sha3_final(struct sha3_ctx *ctx, u8 *out)
{
	__sha3_squeeze(&ctx->ctx, out, ctx->digest_size);
	sha3_zeroize_ctx(ctx);
}

/**
 * shake128_init() - Initialize a context for SHAKE128
 * @ctx: The context to initialize
 *
 * This begins a new SHAKE128 extendable-output function (XOF) computation.
 *
 * Context: Any context.
 */
static inline void shake128_init(struct shake_ctx *ctx)
{
	*ctx = (struct shake_ctx){
		.ctx.block_size	= SHAKE128_BLOCK_SIZE,
		.ctx.padding	= 0x1f,
	};
}

/**
 * shake256_init() - Initialize a context for SHAKE256
 * @ctx: The context to initialize
 *
 * This begins a new SHAKE256 extendable-output function (XOF) computation.
 *
 * Context: Any context.
 */
static inline void shake256_init(struct shake_ctx *ctx)
{
	*ctx = (struct shake_ctx){
		.ctx.block_size	= SHAKE256_BLOCK_SIZE,
		.ctx.padding	= 0x1f,
	};
}

/**
 * shake_update() - Update a SHAKE context with input data
 * @ctx: The context to update; must have been initialized
 * @in: The input data
 * @in_len: Length of the input data in bytes
 *
 * This can be called any number of times to add more input data to SHAKE128 or
 * SHAKE256.  This cannot be called after squeezing has begun.
 *
 * Context: Any context.
 */
static inline void shake_update(struct shake_ctx *ctx,
				const u8 *in, size_t in_len)
{
	__sha3_update(&ctx->ctx, in, in_len);
}

/**
 * shake_squeeze() - Generate output from SHAKE128 or SHAKE256
 * @ctx: The context to squeeze; must have been initialized
 * @out: Where to write the resulting output data
 * @out_len: The amount of data to extract to @out in bytes
 *
 * This may be called multiple times.  A number of consecutive squeezes laid
 * end-to-end will yield the same output as one big squeeze generating the same
 * total amount of output.  More input cannot be provided after squeezing has
 * begun.  After all squeezing has been completed, call shake_zeroize_ctx().
 *
 * Context: Any context.
 */
static inline void shake_squeeze(struct shake_ctx *ctx,
				 u8 *out, size_t out_len)
{
	return __sha3_squeeze(&ctx->ctx, out, out_len);
}

/**
 * sha3_224() - Compute SHA3-224 digest in one shot
 * @in: The input data to be digested
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the digest will be stored
 *
 * Convenience function that computes a SHA3-224 digest.  Use this instead of
 * the incremental API if you're able to provide all the input at once.
 *
 * Context: Any context.
 */
void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE]);

/**
 * sha3_256() - Compute SHA3-256 digest in one shot
 * @in: The input data to be digested
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the digest will be stored
 *
 * Convenience function that computes a SHA3-256 digest.  Use this instead of
 * the incremental API if you're able to provide all the input at once.
 *
 * Context: Any context.
 */
void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE]);

/**
 * sha3_384() - Compute SHA3-384 digest in one shot
 * @in: The input data to be digested
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the digest will be stored
 *
 * Convenience function that computes a SHA3-384 digest.  Use this instead of
 * the incremental API if you're able to provide all the input at once.
 *
 * Context: Any context.
 */
void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE]);

/**
 * sha3_512() - Compute SHA3-512 digest in one shot
 * @in: The input data to be digested
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the digest will be stored
 *
 * Convenience function that computes a SHA3-512 digest.  Use this instead of
 * the incremental API if you're able to provide all the input at once.
 *
 * Context: Any context.
 */
void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE]);

/**
 * shake128() - Compute SHAKE128 in one shot
 * @in: The input data to be used
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the output will be stored
 * @out_len: Length of the output to produce in bytes
 *
 * Convenience function that computes SHAKE128 in one shot.  Use this instead of
 * the incremental API if you're able to provide all the input at once as well
 * as receive all the output at once.  All output lengths are supported.
 *
 * Context: Any context.
 */
void shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len);

/**
 * shake256() - Compute SHAKE256 in one shot
 * @in: The input data to be used
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the output will be stored
 * @out_len: Length of the output to produce in bytes
 *
 * Convenience function that computes SHAKE256 in one shot.  Use this instead of
 * the incremental API if you're able to provide all the input at once as well
 * as receive all the output at once.  All output lengths are supported.
 *
 * Context: Any context.
 */
void shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len);

#endif /* __CRYPTO_SHA3_H__ */
