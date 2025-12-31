/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common values for Ascon-Hash family of algorithms as defined in
 * NIST SP 800-232 https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-232.pdf
 */
#ifndef _CRYPTO_ASCON_HASH_H_
#define _CRYPTO_ASCON_HASH_H_

#include <linux/types.h>

#define ASCON_STATE_SIZE	40
#define ASCON_STATE_WORDS	5

#define ASCON_HASH256_DIGEST_SIZE	32
#define ASCON_HASH256_BLOCK_SIZE	8
#define ASCON_HASH256_RATE		8
#define ASCON_HASH256_IV		0x0000080100CC0002ULL


/*
 * State for Ascon-p[320] permutation: 5 64-bit words
 */
struct ascon_state {
	union {
		__le64 words[ASCON_STATE_WORDS];
		u8 bytes[ASCON_STATE_SIZE];
		u64 native_words[ASCON_STATE_WORDS];
	};
};

/* Internal context */
struct __ascon_hash_ctx {
	struct ascon_state state;
	u8 absorb_offset;
};

/**
 * struct ascon_hash256_ctx - Context for Ascon-Hash256
 * @ctx: private
 */
struct ascon_hash256_ctx {
	struct __ascon_hash_ctx ctx;
};


/**
 * ascon_hash256_init() - Initialize a context for Ascon-Hash256
 * @ctx: The context to initialize
 *
 * This begins a new Ascon-Hash256 message digest computation.
 */
void ascon_hash256_init(struct ascon_hash256_ctx *ctx);

/**
 * ascon_hash256_update() - Update an Ascon-Hash256 digest context with input data
 * @ctx: The context to update; must have been initialized
 * @in: The input data
 * @in_len: Length of the input data in bytes
 */
void ascon_hash256_update(struct ascon_hash256_ctx *ctx, const u8 *in,
			  size_t in_len);

/**
 * ascon_hash256_final() - Finish computing an Ascon-Hash256 message digest
 * @ctx: The context to finalize; must have been initialized
 * @out: (output) The resulting Ascon-Hash256 message digest, matching the init
 *       function that was called.
 */
void ascon_hash256_final(struct ascon_hash256_ctx *ctx,
			 u8 out[ASCON_HASH256_DIGEST_SIZE]);

/**
 * ascon_hash256() - Compute Ascon-Hash256 digest in one shot
 * @in: The input data to be digested
 * @in_len: Length of the input data in bytes
 * @out: The buffer into which the digest will be stored
 *
 * Convenience function that computes an Ascon-Hash256 digest. Use this instead of
 * the incremental API if you are able to provide all the input at once.
 */
void ascon_hash256(const u8 *in, size_t in_len,
		   u8 out[ASCON_HASH256_DIGEST_SIZE]);

#endif
