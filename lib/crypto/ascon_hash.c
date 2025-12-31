// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ascon-Hash library functions
 *
 * Copyright (c) 2025 Rusydi H. Makarim <rusydi.makarim@kriptograf.id>
 */

#include <linux/module.h>
#include <crypto/ascon_hash.h>
#include <crypto/utils.h>


/*
 * The standard of Ascon permutation in NIST SP 800-232 specifies 16 round
 * constants to accomodate potential functionality extensions in the future
 * (see page 2).
 */
static const u64 ascon_p_rndc[] = {
	0x000000000000003cULL, 0x000000000000002dULL, 0x000000000000001eULL,
	0x000000000000000fULL, 0x00000000000000f0ULL, 0x00000000000000e1ULL,
	0x00000000000000d2ULL, 0x00000000000000c3ULL, 0x00000000000000b4ULL,
	0x00000000000000a5ULL, 0x0000000000000096ULL, 0x0000000000000087ULL,
	0x0000000000000078ULL, 0x0000000000000069ULL, 0x000000000000005aULL,
	0x000000000000004bULL,
};


static inline void ascon_round(u64 s[ASCON_STATE_WORDS], u64 C)
{
	u64 t[ASCON_STATE_WORDS];

	// pC
	s[2] ^= C;

	// pS
	s[0] ^= s[4];
	s[4] ^= s[3];
	s[2] ^= s[1];
	t[0] = s[0] ^ (~s[1] & s[2]);
	t[1] = s[1] ^ (~s[2] & s[3]);
	t[2] = s[2] ^ (~s[3] & s[4]);
	t[3] = s[3] ^ (~s[4] & s[0]);
	t[4] = s[4] ^ (~s[0] & s[1]);
	t[1] ^= t[0];
	t[0] ^= t[4];
	t[3] ^= t[2];
	t[2] = ~t[2];

	// pL
	s[0] = t[0] ^ ror64(t[0], 19) ^ ror64(t[0], 28);
	s[1] = t[1] ^ ror64(t[1], 61) ^ ror64(t[1], 39);
	s[2] = t[2] ^ ror64(t[2],  1) ^ ror64(t[2], 6);
	s[3] = t[3] ^ ror64(t[3], 10) ^ ror64(t[3], 17);
	s[4] = t[4] ^ ror64(t[4],  7) ^ ror64(t[4], 41);
}

static inline void ascon_p12_generic(struct ascon_state *state)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(state->words); ++i)
		state->native_words[i] = le64_to_cpu(state->words[i]);

	for (i = 0; i < 12; ++i)
		ascon_round(state->native_words, ascon_p_rndc[16 - 12 + i]);

	for (i = 0; i < ARRAY_SIZE(state->words); ++i)
		state->words[i] = cpu_to_le64(state->native_words[i]);
}

static void __maybe_unused ascon_hash256_absorb_blocks_generic(
	struct ascon_state *state, const u8 *in, size_t nblocks)
{
	do {
		for (size_t i = 0; i < ASCON_HASH256_BLOCK_SIZE; i += 8)
			state->words[i / 8] ^= get_unaligned((__le64 *)&in[i]);
		ascon_p12_generic(state);
		in += ASCON_HASH256_BLOCK_SIZE;
	} while (--nblocks);
}

#define ascon_p12 ascon_p12_generic
#define ascon_hash256_absorb_blocks ascon_hash256_absorb_blocks_generic

void ascon_hash256_init(struct ascon_hash256_ctx *asc_hash256_ctx)
{
	struct __ascon_hash_ctx *ctx = &asc_hash256_ctx->ctx;

	ctx->state.words[0] = ASCON_HASH256_IV;
	ctx->state.words[1] = 0;
	ctx->state.words[2] = 0;
	ctx->state.words[3] = 0;
	ctx->state.words[4] = 0;
	ctx->absorb_offset = 0;
	ascon_p12(&ctx->state);
}
EXPORT_SYMBOL_GPL(ascon_hash256_init);

void ascon_hash256_update(struct ascon_hash256_ctx *asc_hash256_ctx, const u8 *in,
			    size_t in_len)
{
	struct __ascon_hash_ctx *ctx = &asc_hash256_ctx->ctx;
	u8 absorb_offset = ctx->absorb_offset;

	WARN_ON_ONCE(absorb_offset >= ASCON_HASH256_BLOCK_SIZE);

	if (absorb_offset && absorb_offset + in_len >= ASCON_HASH256_BLOCK_SIZE) {
		crypto_xor(&ctx->state.bytes[absorb_offset], in,
			   ASCON_HASH256_BLOCK_SIZE - absorb_offset);
		in += ASCON_HASH256_BLOCK_SIZE - absorb_offset;
		in_len -= ASCON_HASH256_BLOCK_SIZE - absorb_offset;
		ascon_p12(&ctx->state);
		absorb_offset = 0;
	}

	if (in_len >= ASCON_HASH256_BLOCK_SIZE) {
		size_t nblocks = in_len / ASCON_HASH256_BLOCK_SIZE;

		ascon_hash256_absorb_blocks(&ctx->state, in, nblocks);
		in += nblocks * ASCON_HASH256_BLOCK_SIZE;
		in_len -= nblocks * ASCON_HASH256_BLOCK_SIZE;
	}

	if (in_len) {
		crypto_xor(&ctx->state.bytes[absorb_offset], in, in_len);
		absorb_offset += in_len;

	}
	ctx->absorb_offset = absorb_offset;
}
EXPORT_SYMBOL_GPL(ascon_hash256_update);

void ascon_hash256_final(struct ascon_hash256_ctx *asc_hash256_ctx,
			   u8 out[ASCON_HASH256_DIGEST_SIZE])
{
	struct __ascon_hash_ctx *ctx = &asc_hash256_ctx->ctx;

	// padding
	ctx->state.bytes[ctx->absorb_offset] ^= 0x01;
	ascon_p12(&ctx->state);

	// squeezing
	size_t len = ASCON_HASH256_DIGEST_SIZE;

	while (len > ASCON_HASH256_RATE) {
		memcpy(out, ctx->state.bytes, ASCON_HASH256_RATE);
		ascon_p12(&ctx->state);
		out += ASCON_HASH256_RATE;
		len -= ASCON_HASH256_RATE;
	}
	memcpy(out, ctx->state.bytes, ASCON_HASH256_RATE);
	memzero_explicit(asc_hash256_ctx, sizeof(*asc_hash256_ctx));
}
EXPORT_SYMBOL_GPL(ascon_hash256_final);


void ascon_hash256(const u8 *in, size_t in_len,
		   u8 out[ASCON_HASH256_DIGEST_SIZE])
{
	struct ascon_hash256_ctx ctx;

	ascon_hash256_init(&ctx);
	ascon_hash256_update(&ctx, in, in_len);
	ascon_hash256_final(&ctx, out);
}
EXPORT_SYMBOL_GPL(ascon_hash256);

MODULE_DESCRIPTION("Ascon-Hash256 library functions");
MODULE_LICENSE("GPL");
