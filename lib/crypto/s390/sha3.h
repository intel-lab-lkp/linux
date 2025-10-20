/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SHA-3 optimized using the CP Assist for Cryptographic Functions (CPACF)
 *
 * Copyright 2025 Google LLC
 */
#include <asm/cpacf.h>
#include <linux/cpufeature.h>

static __ro_after_init DEFINE_STATIC_KEY_FALSE(have_cpacf_sha3_256);
static __ro_after_init DEFINE_STATIC_KEY_FALSE(have_cpacf_sha3_512);

static void sha3_absorb_blocks(struct __sha3_ctx *ctx,
			       const u8 *data, size_t nblocks)
{
	/*
	 * Note that since the library functions keep the sha3_state in little
	 * endian order, there is no need to convert it to little endian before
	 * invoking CPACF_KIMD_SHA3_*, which also assume little endian order.
	 */
	if (static_branch_likely(&have_cpacf_sha3_256)) {
		if (ctx->block_size == SHA3_224_BLOCK_SIZE) {
			cpacf_kimd(CPACF_KIMD_SHA3_224, &ctx->state, data,
				   nblocks * SHA3_224_BLOCK_SIZE);
			return;
		}
		if (ctx->block_size == SHA3_256_BLOCK_SIZE) {
			cpacf_kimd(CPACF_KIMD_SHA3_256, &ctx->state, data,
				   nblocks * SHA3_256_BLOCK_SIZE);
			return;
		}
	}
	if (static_branch_likely(&have_cpacf_sha3_512)) {
		if (ctx->block_size == SHA3_384_BLOCK_SIZE) {
			cpacf_kimd(CPACF_KIMD_SHA3_384, &ctx->state, data,
				   nblocks * SHA3_384_BLOCK_SIZE);
			return;
		}
		if (ctx->block_size == SHA3_512_BLOCK_SIZE) {
			cpacf_kimd(CPACF_KIMD_SHA3_512, &ctx->state, data,
				   nblocks * SHA3_512_BLOCK_SIZE);
			return;
		}
	}
	sha3_absorb_blocks_generic(ctx, data, nblocks);
}

static void sha3_keccakf(struct sha3_state *state)
{
	if (static_branch_likely(&have_cpacf_sha3_512)) {
		/*
		 * Passing zeroes into any of CPACF_KIMD_SHA3_* gives the plain
		 * Keccak-f permutation, which is what we want here.  Use
		 * SHA3-512 since it has the smallest block size.
		 *
		 * Also, as in sha3_absorb_blocks(), the state needn't be
		 * converted to little endian.  It already is little endian.
		 */
		static const u8 zeroes[SHA3_512_BLOCK_SIZE];

		cpacf_kimd(CPACF_KIMD_SHA3_512, state, zeroes, sizeof(zeroes));
	} else {
		sha3_keccakf_generic(state);
	}
}

#define sha3_mod_init_arch sha3_mod_init_arch
static void sha3_mod_init_arch(void)
{
	if (cpu_have_feature(S390_CPU_FEATURE_MSA)) {
		if (cpacf_query_func(CPACF_KIMD, CPACF_KIMD_SHA3_256))
			static_branch_enable(&have_cpacf_sha3_256);
		if (cpacf_query_func(CPACF_KIMD, CPACF_KIMD_SHA3_512))
			static_branch_enable(&have_cpacf_sha3_512);
	}
}
