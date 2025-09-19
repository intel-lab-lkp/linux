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
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <crypto/sha3.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#if 0
static void sha3_dump_state(const struct sha3_ctx *ctx, const char *prefix)
{
	const __be64 *p = (const __be64 *)ctx->state.st;

	for (int i = 0; i < 6; i++) {
		pr_info("SHA3 %s %016llx %016llx %016llx %016llx\n",
			prefix,
			be64_to_cpu(p[0]), be64_to_cpu(p[1]),
			be64_to_cpu(p[2]), be64_to_cpu(p[3]));
		p += 4;
	}
	pr_info("SHA3 %s %016llx\n", prefix, be64_to_cpu(p[0]));
}
#endif

/*
 * On some 32-bit architectures (h8300), GCC ends up using
 * over 1 KB of stack if we inline the round calculation into the loop
 * in keccakf(). On the other hand, on 64-bit architectures with plenty
 * of [64-bit wide] general purpose registers, not inlining it severely
 * hurts performance. So let's use 64-bitness as a heuristic to decide
 * whether to inline or not.
 */
#ifdef CONFIG_64BIT
#define SHA3_INLINE	inline
#else
#define SHA3_INLINE	noinline
#endif

#define KECCAK_ROUNDS 24

static const u64 keccakf_rndc[24] = {
	0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
	0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
	0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
	0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
	0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
	0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
	0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
	0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

/* update the state with given number of rounds */

static SHA3_INLINE void keccakf_round(u64 st[25])
{
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
}

static void sha3_be_swap_state(u64 st[25])
{
#ifdef __BIG_ENDIAN
	for (int i = 0; i < 25; i++)
		st[i] = __builtin_bswap64(st[i]);
#endif
}

/*
 * Perform the mixing step.
 */
static void sha3_keccakf_generic(u64 st[25])
{
	int round;

	sha3_be_swap_state(st);

	for (round = 0; round < KECCAK_ROUNDS; round++) {
		keccakf_round(st);
		/* Iota */
		st[0] ^= keccakf_rndc[round];
	}

	sha3_be_swap_state(st);
}

static void sha3_absorb_block_generic(struct sha3_ctx *ctx, const u8 *data)
{
	struct sha3_state *state = &ctx->state;
	unsigned int bsize = ctx->block_size;

	for (int i = 0; i < bsize / 8; i++)
		state->st[i] ^= get_unaligned((u64 *)(data + 8 * i));
	sha3_keccakf_generic(state->st);
}

/*
 * Perform rounds of XOR'ing whole blocks of data into the state buffer and
 * then performing a keccak mix step.
 */
static void
sha3_absorb_blocks_generic(struct sha3_ctx *ctx, const u8 *data, size_t nblocks)
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
static void sha3_absorb_xorle(struct sha3_ctx *ctx, const u8 *data,
			      unsigned int partial, unsigned int len)
{
	u8 *buf = (u8 *)ctx->state.st;

	buf += partial;
	for (int i = 0; i < len; i++)
		*buf++ ^= *data++;
}

/**
 * sha3_update() - Update a SHA3 context of any type with message data
 * @ctx: the context to update; must have been initialized
 * @data: the message data
 * @len: the data length in bytes
 *
 * This can be called any number of times to perform the "keccak sponge
 * absorbing" phase.
 *
 * It may be called again after @sha3_final() has been called to add more data
 * to the hash.  Note, however, that finalising a hash modifies the state, so
 * {update,final,update} is not equivalent to {update,update}.
 */
void sha3_update(struct sha3_ctx *ctx, const u8 *data, unsigned int len)
{
	unsigned int partial = ctx->partial;
	unsigned int bsize = ctx->block_size;

	if (partial && partial + len >= bsize) {
		sha3_absorb_xorle(ctx, data, partial, bsize - partial);
		len  -= bsize - partial;
		data += bsize - partial;
		sha3_keccakf(ctx->state.st);
		ctx->partial = 0;
	}

	if (len >= bsize) {
		size_t nblocks = len / bsize;

		if (nblocks) {
			sha3_absorb_blocks(ctx, data, nblocks);
			data += nblocks * bsize;
			len  -= nblocks * bsize;
		}
	}

	if (len) {
		sha3_absorb_xorle(ctx, data, ctx->partial, len);
		ctx->partial += len;
	}
	ctx->end_marked = false;
}
EXPORT_SYMBOL_GPL(sha3_update);

/**
 * sha3_squeeze() - Finalize a SHA3 digest of any type and extract the digest
 * @ctx: the context to finalize; must have been initialized
 * @out: Where to write the resulting message digest
 * @out_size: The amount of digest to extract to @out
 *
 * Finish the computation of a SHA3 message digest of any type and perform the
 * "Keccak sponge squeezing" phase.  @out_size amount of digest is written to
 * @out buffer.
 *
 * This may be called multiple times to extract continuations of the digest.
 * Note that, for example, two consecutive 16-byte squeezes laid end-to-end
 * will yield the same as one 32-byte squeeze.
 *
 * The state will have the end marker added again if any new updates have
 * happened since the last time it was squeezed.
 */
void sha3_squeeze(struct sha3_ctx *ctx, u8 *out, size_t out_size)
{
	unsigned int squeeze_offset = ctx->squeeze_offset;
	unsigned int digest_size = out_size;
	unsigned int bsize = ctx->block_size;
	u8 *p = (u8 *)ctx->state.st, end_marker = 0x80;

	if (!ctx->end_marked) {
		sha3_absorb_xorle(ctx, &ctx->padding, ctx->partial, 1);
		sha3_absorb_xorle(ctx, &end_marker, bsize - 1, 1);
		ctx->end_marked = true;
	}

	for (;;) {
		if (squeeze_offset == 0) {
			sha3_keccakf(ctx->state.st);
		}

		unsigned int part = umin(digest_size, bsize - squeeze_offset);

		if (part > 0) {
			memcpy(out, p + squeeze_offset, part);
			digest_size -= part;
			out += part;
			squeeze_offset += part;
		}
		if (!digest_size)
			break;
		if (squeeze_offset >= bsize)
			squeeze_offset -= bsize;
	}

	ctx->squeeze_offset = squeeze_offset;
}
EXPORT_SYMBOL_GPL(sha3_squeeze);

/**
 * sha3_init() - Initialize a SHA3 context for a new message
 * @ctx: the context to initialize
 *
 * Initialize a SHA3 context for any size of SHA-3 digest.
 */
void sha3_init(struct sha3_ctx *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
}
EXPORT_SYMBOL_GPL(sha3_init);

/**
 * sha3_224() - Convenience wrapper to digest a simple buffer as SHA3-224
 * @in: The data to be digested
 * @in_len: The amount of data to be digested
 * @out: The buffer into which the digest will be stored (size not checked)
 */
void sha3_224(const u8 *in, size_t in_len, u8 out[SHA3_224_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_224_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL(sha3_224);

/**
 * sha3_256() - Convenience wrapper to digest a simple buffer as SHA3-256
 * @in: The data to be digested
 * @in_len: The amount of data to be digested
 * @out: The buffer into which the digest will be stored (size not checked)
 */
void sha3_256(const u8 *in, size_t in_len, u8 out[SHA3_256_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_256_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL(sha3_256);

/**
 * sha3_384() - Convenience wrapper to digest a simple buffer as SHA3-384
 * @in: The data to be digested
 * @in_len: The amount of data to be digested
 * @out: The buffer into which the digest will be stored (size not checked)
 */
void sha3_384(const u8 *in, size_t in_len, u8 out[SHA3_384_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_384_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL(sha3_384);

/**
 * sha3_512() - Convenience wrapper to digest a simple buffer as SHA3-512
 * @in: The data to be digested
 * @in_len: The amount of data to be digested
 * @out: The buffer into which the digest will be stored (size not checked)
 */
void sha3_512(const u8 *in, size_t in_len, u8 out[SHA3_512_DIGEST_SIZE])
{
	struct sha3_ctx ctx;

	sha3_512_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_final(&ctx, out);
}
EXPORT_SYMBOL(sha3_512);

/**
 * shake128() - Convenience wrapper to digest a simple buffer as SHAKE128
 * @in: The data to be digested
 * @in_len: The amount of data to be digested
 * @out: The buffer into which the digest will be stored
 * @out_len: The size of the digest desired (variable length)
 */
void shake128(const u8 *in, size_t in_len, u8 *out, size_t out_len)
{
	struct sha3_ctx ctx;

	shake128_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_squeeze(&ctx, out, out_len);
	sha3_clear(&ctx);
}
EXPORT_SYMBOL(shake128);

/**
 * shake256() - Convenience wrapper to digest a simple buffer as SHAKE256
 * @in: The data to be digested
 * @in_len: The amount of data to be digested
 * @out: The buffer into which the digest will be stored
 * @out_len: The size of the digest desired (variable length)
 */
void shake256(const u8 *in, size_t in_len, u8 *out, size_t out_len)
{
	struct sha3_ctx ctx;

	shake256_init(&ctx);
	sha3_update(&ctx, in, in_len);
	sha3_squeeze(&ctx, out, out_len);
	sha3_clear(&ctx);
}
EXPORT_SYMBOL(shake256);

/*
 * Do a quick test using SHAKE256 and a 200 byte digest.
 */
static const u8 sha3_sample1[] __initconst =
	"The quick red fox jumped over the lazy brown dog!\n"
	"The quick red fox jumped over the lazy brown dog!\n"
	"The quick red fox jumped over the lazy brown dog!\n"
	"The quick red fox jumped over the lazy brown dog!\n";
static const u8 sha3_sample2[] __initconst =
	"hello\n";
static const u8 sha3_sample_shake256_200_step1[] __initconst = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-before guard */
	0xab, 0x06, 0xd4, 0xf9, 0x8b, 0xfd, 0xb2, 0xc4, 0xfe, 0xf1, 0xcc, 0xe2,
	0x40, 0x45, 0xdd, 0x15, 0xcb, 0xdd, 0x02, 0x8d, 0xb7, 0x9f, 0x1e, 0x67,
	0xd6, 0x7f, 0x98, 0x5e, 0x1b, 0x19, 0xf8, 0x01, 0x43, 0x82, 0xcb, 0xd8,
	0x5d, 0x21, 0x64, 0xa8, 0x80, 0xc9, 0x22, 0xe5, 0x07, 0xaf, 0xe2, 0x5d,
	0xcd, 0xc6, 0x23, 0x36, 0x2b, 0xc7, 0xc7, 0x7d, 0x09, 0x9d, 0x68, 0x05,
	0xe4, 0x62, 0x63, 0x1b, 0x67, 0xbc, 0xf8, 0x95, 0x07, 0xd2, 0xe4, 0xd0,
	0xba, 0xa2, 0x67, 0xf5, 0xe3, 0x15, 0xbc, 0x85, 0xa1, 0x50, 0xd6, 0x6f,
	0x6f, 0xd4, 0x54, 0x4c, 0x3f, 0x4f, 0xe5, 0x1f, 0xb7, 0x00, 0x27, 0xfc,
	0x15, 0x33, 0xc2, 0xf9, 0xb3, 0x4b, 0x9e, 0x81, 0xe5, 0x96, 0xbe, 0x05,
	0x6c, 0xac, 0xf9, 0x9f, 0x65, 0x36, 0xbb, 0x11, 0x47, 0x6d, 0xf6, 0x8f,
	0x9f, 0xa2, 0x77, 0x37, 0x3b, 0x18, 0x77, 0xcf, 0x65, 0xc5, 0xa1, 0x7e,
	0x2c, 0x0e, 0x71, 0xf0, 0x4d, 0x18, 0x67, 0xb9, 0xc4, 0x8c, 0x64, 0x3b,
	0x4b, 0x45, 0xea, 0x16, 0xb2, 0x4a, 0xc5, 0xf5, 0x85, 0xdc, 0xd2, 0xd9,
	0x13, 0x77, 0xb3, 0x19, 0xd9, 0x8c, 0x9f, 0x28, 0xe7, 0x64, 0x91, 0x0f,
	0x6f, 0x32, 0xbf, 0xa8, 0xa8, 0xa3, 0xff, 0x99, 0x0e, 0x0b, 0x62, 0x50,
	0xf8, 0x3a, 0xc2, 0xf5, 0x98, 0x21, 0xeb, 0x9d, 0xe8, 0x45, 0xf4, 0x46,
	0x1e, 0x8b, 0xbd, 0x10, 0x59, 0x2c, 0x87, 0xe2,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-after guard */
};
static const u8 sha3_sample_shake256_200_step2[] __initconst = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-before guard */
	0x5b, 0x43, 0x98, 0x3a, 0x4a, 0x5f, 0xa9, 0x14, 0xa5, 0x98, 0x26, 0xe2,
	0xed, 0x45, 0x6a, 0x1d, 0x61, 0x24, 0xf5, 0x0c, 0xef, 0xda, 0xc2, 0x8a,
	0x30, 0x0e, 0x03, 0xe5, 0x67, 0xdd, 0x7e, 0x9f, 0xa0, 0xa4, 0x07, 0x63,
	0xdc, 0x6b, 0x7e, 0xbd, 0xd7, 0x7d, 0x7a, 0x6d, 0x55, 0x03, 0x02, 0x18,
	0x12, 0x5d, 0xf9, 0x21, 0xc8, 0x78, 0x69, 0x7c, 0x64, 0x39, 0xfd, 0xf4,
	0xd6, 0x06, 0xe6, 0xd8, 0x6f, 0xaa, 0x04, 0x5b, 0x40, 0xf3, 0x96, 0xb2,
	0xb5, 0xd0, 0xb5, 0x43, 0x50, 0x9c, 0x08, 0xd6, 0x54, 0x8e, 0x8c, 0x85,
	0xc2, 0x34, 0xce, 0x0c, 0x24, 0x31, 0x6f, 0x49, 0xec, 0x3d, 0x13, 0x1f,
	0x36, 0x0a, 0x14, 0xa6, 0x5d, 0x51, 0x9a, 0x90, 0x1f, 0xf5, 0x1f, 0x61,
	0xb7, 0x65, 0x64, 0x2a, 0x00, 0x07, 0xe4, 0x56, 0x80, 0x5c, 0xfa, 0x03,
	0xc4, 0x97, 0xc1, 0x09, 0x35, 0xa2, 0x55, 0x72, 0x28, 0xe5, 0xb6, 0xef,
	0x8e, 0xf4, 0xc2, 0x82, 0x22, 0xc7, 0x23, 0xac, 0xcb, 0xc1, 0x03, 0x52,
	0x46, 0x9c, 0x17, 0xe0, 0xa3, 0x1b, 0x59, 0x9f, 0x01, 0xef, 0x5b, 0x46,
	0xb2, 0x4b, 0x98, 0x6b, 0x32, 0x52, 0xe3, 0x29, 0x36, 0x8f, 0x66, 0x98,
	0x5f, 0x6a, 0xa2, 0xf4, 0x68, 0x13, 0x5c, 0x94, 0xe4, 0x22, 0xb6, 0x83,
	0xa0, 0xd7, 0xa3, 0xda, 0xa4, 0x84, 0x0c, 0xf6, 0xa2, 0xa4, 0x0e, 0x08,
	0x6d, 0x2b, 0xd2, 0x31, 0x77, 0x36, 0xae, 0x53,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-after guard */
};

static const u8 sha3_sample_shake256_200_step3[] __initconst = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-before guard */
	0x61, 0x0a, 0x5e, 0xdf, 0xf1, 0x29, 0xae, 0x82, 0xab, 0x57, 0xa8, 0x1b,
	0x4e, 0x7c, 0xb9, 0x14, 0x4a, 0x32, 0x7d, 0x82, 0xdc, 0xc2, 0x48, 0x1c,
	0xf4, 0xa4, 0x18, 0xd5, 0x97, 0x35, 0x9a, 0x25, 0x0f, 0x5f, 0x1b, 0x04,
	0xf1, 0x09, 0x2a, 0xe8, 0xb6, 0xa7, 0xe1, 0x90, 0xb6, 0x4d, 0x96, 0xf1,
	0x7d, 0x4d, 0xb0, 0x4f, 0x44, 0xaf, 0x16, 0x4e, 0x63, 0xce, 0x46, 0x4c,
	0x76, 0x18, 0xbe, 0x5f, 0xf4, 0x35, 0xef, 0x1f, 0xb1, 0x97, 0x94, 0x70,
	0x96, 0x2f, 0xa2, 0x1b, 0xd6, 0x02, 0x51, 0x88, 0x33, 0x2b, 0x54, 0xb9,
	0x44, 0xb4, 0xab, 0x6f, 0xeb, 0xfc, 0xe5, 0xee, 0xe3, 0x77, 0x91, 0xed,
	0x3a, 0x4e, 0x60, 0x00, 0x44, 0xd1, 0xc7, 0x4a, 0x54, 0x77, 0x71, 0x95,
	0x53, 0x88, 0x6b, 0x1e, 0x0f, 0xfd, 0x62, 0x02, 0xa7, 0x8e, 0x05, 0x6d,
	0x21, 0x8f, 0x97, 0x20, 0xa0, 0xd7, 0xcf, 0xd8, 0x54, 0xec, 0x50, 0x72,
	0x07, 0xb8, 0x9c, 0x76, 0xdb, 0x12, 0x00, 0xd2, 0x2e, 0x93, 0xee, 0xb9,
	0x6a, 0x28, 0x5a, 0x46, 0x87, 0x90, 0xd5, 0xd6, 0x1d, 0x14, 0x0e, 0x16,
	0xf1, 0x2c, 0xed, 0x7f, 0x28, 0x34, 0x8c, 0x2b, 0x96, 0x03, 0x80, 0x80,
	0x9f, 0xc8, 0xf4, 0x2c, 0x53, 0xe0, 0x4b, 0x7b, 0xf4, 0x19, 0x8a, 0xc5,
	0xb3, 0x21, 0x17, 0xce, 0xdb, 0xbf, 0xb7, 0x6b, 0x9a, 0xb5, 0x19, 0x89,
	0x4c, 0x54, 0x28, 0x32, 0xe6, 0x85, 0xfa, 0x8f,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-after guard */
};
static const u8 sha3_sample_shake256_200_step4[] __initconst = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-before guard */
	0x83, 0x99, 0xc6, 0xda, 0x75, 0x79, 0x8e, 0x47, 0x06, 0xad, 0x19, 0xcb,
	0x47, 0x61, 0x25, 0x6a, 0x8c, 0xa4, 0x7e, 0x74, 0xba, 0xbd, 0xda, 0xb6,
	0x3b, 0x08, 0x48, 0x0f, 0x55, 0xd5, 0x85, 0x78, 0x5b, 0xd4, 0x31, 0xcb,
	0x59, 0xff, 0x93, 0xff, 0xf6, 0x65, 0x4c, 0xf7, 0x6e, 0x4b, 0xef, 0x4d,
	0x0e, 0x43, 0x8a, 0x2b, 0xed, 0x10, 0x26, 0x68, 0x12, 0x63, 0xed, 0x7a,
	0x38, 0x0a, 0xa5, 0xd0, 0x79, 0x26, 0x75, 0xef, 0xce, 0xad, 0x6c, 0x12,
	0x52, 0x33, 0xec, 0xe8, 0xe1, 0x89, 0x2f, 0x0f, 0x29, 0xb0, 0xf6, 0xff,
	0x54, 0x11, 0xb2, 0x6b, 0x22, 0xb3, 0x48, 0x01, 0xa5, 0xcf, 0x29, 0xb7,
	0xaf, 0x8c, 0xec, 0x1e, 0x75, 0x3e, 0xff, 0xfb, 0x31, 0xb8, 0xf6, 0xab,
	0xae, 0xac, 0xec, 0xed, 0x27, 0x0b, 0x79, 0x10, 0x4f, 0x87, 0xe8, 0x43,
	0x28, 0x94, 0x09, 0xca, 0x48, 0x63, 0x65, 0x61, 0x86, 0x83, 0x33, 0x30,
	0x02, 0x6d, 0xf4, 0xef, 0x3c, 0x1a, 0x47, 0x8a, 0x25, 0x90, 0x31, 0x39,
	0x95, 0x1d, 0x6f, 0x11, 0x5c, 0x0c, 0x72, 0xe6, 0x1b, 0xe1, 0x60, 0x45,
	0x79, 0x89, 0x39, 0x48, 0x31, 0x4c, 0xc4, 0xd1, 0x08, 0x12, 0xf3, 0x5f,
	0x84, 0x8c, 0x86, 0xba, 0xe5, 0xf1, 0x24, 0x61, 0x2f, 0xef, 0x17, 0x16,
	0x4a, 0x29, 0xc0, 0xc6, 0x38, 0x47, 0x3a, 0x11, 0xc5, 0x7d, 0x62, 0x85,
	0x9b, 0x18, 0x92, 0x4c, 0x12, 0x92, 0x9c, 0x0b,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* Write-after guard */
};

static void __init sha3_check_digest(const u8 *digest, const u8 *sample_digest,
				    int digest_len)
{
	if (memcmp(digest, sample_digest, digest_len) != 0) {
		pr_err("SHAKE256(200) failed\n");
		for (int i = 0; i < digest_len;) {
			int part = min(digest_len - i, 32);

			pr_err("%*phN\n", part, digest + i);
			i += part;
		}
	}
}

static int __init sha3_mod_init(void)
{
#define dsize 200
	struct sha3_ctx ctx;
	u8 digest[8 + dsize + 8] = {};

#ifdef sha3_mod_init_arch
	sha3_mod_init_arch();
#endif

	BUILD_BUG_ON(sizeof(digest) != sizeof(sha3_sample_shake256_200_step1) ||
		     sizeof(digest) != sizeof(sha3_sample_shake256_200_step2) ||
		     sizeof(digest) != sizeof(sha3_sample_shake256_200_step3) ||
		     sizeof(digest) != sizeof(sha3_sample_shake256_200_step4));

	shake256_init(&ctx);
	sha3_update(&ctx, sha3_sample1, sizeof(sha3_sample1) - 1);
	sha3_squeeze(&ctx, digest + 8, dsize);
	sha3_check_digest(digest, sha3_sample_shake256_200_step1, sizeof(digest));

	sha3_squeeze(&ctx, digest + 8, dsize);
	sha3_check_digest(digest, sha3_sample_shake256_200_step2, sizeof(digest));

	sha3_squeeze(&ctx, digest + 8, dsize);
	sha3_check_digest(digest, sha3_sample_shake256_200_step3, sizeof(digest));

	sha3_update(&ctx, sha3_sample2, sizeof(sha3_sample2) - 1);
	sha3_squeeze(&ctx, digest + 8, dsize);
	sha3_check_digest(digest, sha3_sample_shake256_200_step4, sizeof(digest));
	return 0;
}
subsys_initcall(sha3_mod_init);

#ifdef sha3_mod_init_arch
static void __exit sha3_mod_exit(void)
{
}
module_exit(sha3_mod_exit);
#endif

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SHA-3 Secure Hash Algorithm");
