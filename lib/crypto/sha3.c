// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cryptographic API.
 *
 * SHA-3, as specified in
 * https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.202.pdf
 *
 * SHA-3 code by Jeff Garzik <jeff@garzik.org>
 *               Ard Biesheuvel <ard.biesheuvel@linaro.org>
 *		 David Howells <dhowells@redhat.com>
 *
 * See also Documentation/crypto/sha3.rst
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <crypto/sha3.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/unaligned.h>
#include "fips.h"

/*
 * On some 32-bit architectures, such as h8300, GCC ends up using over 1 KB of
 * stack if the round calculation gets inlined into the loop in
 * sha3_keccakf_rounds_generic().  On the other hand, on 64-bit architectures
 * with plenty of [64-bit wide] general purpose registers, not inlining it
 * severely hurts performance.  So let's use 64-bitness as a heuristic to
 * decide whether to inline or not.
 */
#ifdef CONFIG_64BIT
#define SHA3_INLINE	inline
#else
#define SHA3_INLINE	noinline
#endif

#define SHA3_KECCAK_ROUNDS 24

static const u64 sha3_keccakf_rndc[24] = {
	0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
	0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
	0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
	0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
	0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
	0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
	0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
	0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

/*
 * Perform a single round of Keccak mixing.
 */
static SHA3_INLINE void sha3_keccakf_one_round_generic(struct sha3_state *state,
						       int round)
{
	u64 *st = state->st;
	u64 t[5], tt, bc[5];

	/* Theta */
	bc[0] = st[0] ^ st[5] ^ st[10] ^ st[15] ^ st[20];
	bc[1] = st[1] ^ st[6] ^ st[11] ^ st[16] ^ st[21];
	bc[2] = st[2] ^ st[7] ^ st[12] ^ st[17] ^ st[22];
	bc[3] = st[3] ^ st[8] ^ st[13] ^ st[18] ^ st[23];
	bc[4] = st[4] ^ st[9] ^ st[14] ^ st[19] ^ st[24];

	t[0] = bc[4] ^ rol64(bc[1], 1);
	t[1] = bc[0] ^ rol64(bc[2], 1);
	t[2] = bc[1] ^ rol64(bc[3], 1);
	t[3] = bc[2] ^ rol64(bc[4], 1);
	t[4] = bc[3] ^ rol64(bc[0], 1);

	st[0] ^= t[0];

	/* Rho Pi */
	tt = st[1];
	st[ 1] = rol64(st[ 6] ^ t[1], 44);
	st[ 6] = rol64(st[ 9] ^ t[4], 20);
	st[ 9] = rol64(st[22] ^ t[2], 61);
	st[22] = rol64(st[14] ^ t[4], 39);
	st[14] = rol64(st[20] ^ t[0], 18);
	st[20] = rol64(st[ 2] ^ t[2], 62);
	st[ 2] = rol64(st[12] ^ t[2], 43);
	st[12] = rol64(st[13] ^ t[3], 25);
	st[13] = rol64(st[19] ^ t[4],  8);
	st[19] = rol64(st[23] ^ t[3], 56);
	st[23] = rol64(st[15] ^ t[0], 41);
	st[15] = rol64(st[ 4] ^ t[4], 27);
	st[ 4] = rol64(st[24] ^ t[4], 14);
	st[24] = rol64(st[21] ^ t[1],  2);
	st[21] = rol64(st[ 8] ^ t[3], 55);
	st[ 8] = rol64(st[16] ^ t[1], 45);
	st[16] = rol64(st[ 5] ^ t[0], 36);
	st[ 5] = rol64(st[ 3] ^ t[3], 28);
	st[ 3] = rol64(st[18] ^ t[3], 21);
	st[18] = rol64(st[17] ^ t[2], 15);
	st[17] = rol64(st[11] ^ t[1], 10);
	st[11] = rol64(st[ 7] ^ t[2],  6);
	st[ 7] = rol64(st[10] ^ t[0],  3);
	st[10] = rol64(    tt ^ t[1],  1);

	/* Chi */
	bc[ 0] = ~st[ 1] & st[ 2];
	bc[ 1] = ~st[ 2] & st[ 3];
	bc[ 2] = ~st[ 3] & st[ 4];
	bc[ 3] = ~st[ 4] & st[ 0];
	bc[ 4] = ~st[ 0] & st[ 1];
	st[ 0] ^= bc[ 0];
	st[ 1] ^= bc[ 1];
	st[ 2] ^= bc[ 2];
	st[ 3] ^= bc[ 3];
	st[ 4] ^= bc[ 4];

	bc[ 0] = ~st[ 6] & st[ 7];
	bc[ 1] = ~st[ 7] & st[ 8];
	bc[ 2] = ~st[ 8] & st[ 9];
	bc[ 3] = ~st[ 9] & st[ 5];
	bc[ 4] = ~st[ 5] & st[ 6];
	st[ 5] ^= bc[ 0];
	st[ 6] ^= bc[ 1];
	st[ 7] ^= bc[ 2];
	st[ 8] ^= bc[ 3];
	st[ 9] ^= bc[ 4];

	bc[ 0] = ~st[11] & st[12];
	bc[ 1] = ~st[12] & st[13];
	bc[ 2] = ~st[13] & st[14];
	bc[ 3] = ~st[14] & st[10];
	bc[ 4] = ~st[10] & st[11];
	st[10] ^= bc[ 0];
	st[11] ^= bc[ 1];
	st[12] ^= bc[ 2];
	st[13] ^= bc[ 3];
	st[14] ^= bc[ 4];

	bc[ 0] = ~st[16] & st[17];
	bc[ 1] = ~st[17] & st[18];
	bc[ 2] = ~st[18] & st[19];
	bc[ 3] = ~st[19] & st[15];
	bc[ 4] = ~st[15] & st[16];
	st[15] ^= bc[ 0];
	st[16] ^= bc[ 1];
	st[17] ^= bc[ 2];
	st[18] ^= bc[ 3];
	st[19] ^= bc[ 4];

	bc[ 0] = ~st[21] & st[22];
	bc[ 1] = ~st[22] & st[23];
	bc[ 2] = ~st[23] & st[24];
	bc[ 3] = ~st[24] & st[20];
	bc[ 4] = ~st[20] & st[21];
	st[20] ^= bc[ 0];
	st[21] ^= bc[ 1];
	st[22] ^= bc[ 2];
	st[23] ^= bc[ 3];
	st[24] ^= bc[ 4];

	/* Iota */
	state->st[0] ^= sha3_keccakf_rndc[round];
}

static void sha3_keccakf_rounds_generic(struct sha3_state *state)
{
	for (int round = 0; round < SHA3_KECCAK_ROUNDS; round++)
		sha3_keccakf_one_round_generic(state, round);
}

/*
 * Byteswap the state buckets to CPU-endian if we're not on a little-endian
 * machine for the duration of the Keccak mixing function.  Note that these
 * loops are no-ops on LE machines and will be optimised away.
 */
static void sha3_keccakf_generic(struct sha3_state *state)
{
	for (int  i = 0; i < ARRAY_SIZE(state->st); i++)
		le64_to_cpus(&state->st[i]);

	sha3_keccakf_rounds_generic(state);

	for (int  i = 0; i < ARRAY_SIZE(state->st); i++)
		cpu_to_le64s(&state->st[i]);
}

static void sha3_absorb_block_generic(struct __sha3_ctx *ctx, const u8 *data)
{
	struct sha3_state *state = &ctx->state;
	size_t bsize = ctx->block_size;

	for (size_t i = 0; i < bsize / 8; i++)
		state->st[i] ^= get_unaligned((u64 *)(data + 8 * i));
	sha3_keccakf_generic(state);
}

/*
 * Perform rounds of XOR'ing whole blocks of data into the state buffer and
 * then performing a keccak mix step.
 */
static void sha3_absorb_blocks_generic(struct __sha3_ctx *ctx,
				       const u8 *data, size_t nblocks)
{
	do {
		sha3_absorb_block_generic(ctx, data);
		data += ctx->block_size;
	} while (--nblocks);
}

#ifdef CONFIG_CRYPTO_LIB_SHA3_ARCH
#include "sha3.h" /* $(SRCARCH)/sha3.h */
#else
#define sha3_keccakf		sha3_keccakf_generic
#define sha3_absorb_blocks	sha3_absorb_blocks_generic
#endif

/*
 * XOR in partial data that's insufficient to fill a whole block.
 */
static void sha3_absorb_xorle(struct __sha3_ctx *ctx, const u8 *data,
			      size_t partial, size_t len)
{
	u8 *buf = (u8 *)ctx->state.st;

	buf += partial;
	for (size_t i = 0; i < len; i++)
		*buf++ ^= *data++;
}

void __sha3_update(struct __sha3_ctx *ctx, const u8 *data, size_t len)
{
	size_t absorb_offset = ctx->absorb_offset;
	size_t bsize = ctx->block_size;

	WARN_ON_ONCE(ctx->end_marked);

	if (absorb_offset && absorb_offset + len >= bsize) {
		sha3_absorb_xorle(ctx, data, absorb_offset, bsize - absorb_offset);
		len  -= bsize - absorb_offset;
		data += bsize - absorb_offset;
		sha3_keccakf(&ctx->state);
		ctx->absorb_offset = 0;
	}

	if (len >= bsize) {
		size_t nblocks = len / bsize;

		sha3_absorb_blocks(ctx, data, nblocks);
		data += nblocks * bsize;
		len  -= nblocks * bsize;
	}

	if (len) {
		sha3_absorb_xorle(ctx, data, ctx->absorb_offset, len);
		ctx->absorb_offset += len;
	}
}
EXPORT_SYMBOL_GPL(__sha3_update);

void __sha3_squeeze(struct __sha3_ctx *ctx, u8 *out, size_t out_len)
{
	size_t squeeze_offset = ctx->squeeze_offset;
	size_t bsize = ctx->block_size;
	u8 *p = (u8 *)ctx->state.st, end_marker = 0x80;

	if (!ctx->end_marked) {
		sha3_absorb_xorle(ctx, &ctx->padding, ctx->absorb_offset, 1);
		sha3_absorb_xorle(ctx, &end_marker, bsize - 1, 1);
		ctx->end_marked = true;
	}

	for (;;) {
		if (squeeze_offset == 0)
			sha3_keccakf(&ctx->state);

		size_t part = umin(out_len, bsize - squeeze_offset);

		if (part > 0) {
			memcpy(out, p + squeeze_offset, part);
			out_len -= part;
			out += part;
			squeeze_offset += part;
		}
		if (!out_len)
			break;
		if (squeeze_offset >= bsize)
			squeeze_offset = 0;
	}

	ctx->squeeze_offset = squeeze_offset;
}
EXPORT_SYMBOL_GPL(__sha3_squeeze);

void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_224_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL_GPL(sha3_224);

void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_256_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL_GPL(sha3_256);

void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_384_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL_GPL(sha3_384);

void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_512_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL_GPL(sha3_512);

void shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len)
{
	struct shake_ctx ctx;

	shake128_init(&ctx);
	shake_update(&ctx, in, in_len);
	shake_squeeze(&ctx, out, out_len);
	shake_zeroize_ctx(&ctx);
}
EXPORT_SYMBOL_GPL(shake128);

void shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len)
{
	struct shake_ctx ctx;

	shake256_init(&ctx);
	shake_update(&ctx, in, in_len);
	shake_squeeze(&ctx, out, out_len);
	shake_zeroize_ctx(&ctx);
}
EXPORT_SYMBOL_GPL(shake256);

#if defined(sha3_mod_init_arch) || defined(CONFIG_CRYPTO_FIPS)
static int __init sha3_mod_init(void)
{
#ifdef sha3_mod_init_arch
	sha3_mod_init_arch();
#endif
	if (fips_enabled) {
		/*
		 * FIPS cryptographic algorithm self-test.  As per the FIPS
		 * Implementation Guidance, testing any SHA-3 algorithm
		 * satisfies the test requirement for all of them.
		 */
		u8 hash[SHA3_256_DIGEST_SIZE];

		sha3_256(fips_test_data, sizeof(fips_test_data), hash);
		if (memcmp(fips_test_sha3_256_value, hash, sizeof(hash)) != 0)
			panic("sha3: FIPS self-test failed\n");
	}
	return 0;
}
subsys_initcall(sha3_mod_init);

static void __exit sha3_mod_exit(void)
{
}
module_exit(sha3_mod_exit);
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SHA-3 Secure Hash Algorithm");
