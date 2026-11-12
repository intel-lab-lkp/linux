/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AEGIS-128X4 optimized using VAES and AVX-512
 *
 * Copyright 2026 Antonio Quartulli <antonio@mandelbit.com>
 */

#include <asm/fpu/api.h>
#include <linux/jump_label.h>
#include <linux/sizes.h>

static __ro_after_init DEFINE_STATIC_KEY_FALSE(have_vaes_avx512);

asmlinkage void
aegis128x4_init_vaes_avx512(union aegis128x4_state *state,
			    const u8 key[AEGIS128X4_KEY_SIZE],
			    const u8 nonce[AEGIS128X4_NONCE_SIZE]);
asmlinkage void aegis128x4_absorb_vaes_avx512(union aegis128x4_state *state,
					      const u8 *src, size_t len);
asmlinkage void aegis128x4_enc_vaes_avx512(union aegis128x4_state *state,
					   u8 *dst, const u8 *src, size_t len);
asmlinkage void aegis128x4_dec_vaes_avx512(union aegis128x4_state *state,
					   u8 *dst, const u8 *src, size_t len);
asmlinkage void aegis128x4_final_vaes_avx512(union aegis128x4_state *state,
					     u8 *tag, u64 assoclen,
					     u64 cryptlen,
					     unsigned int authsize);
asmlinkage void
aegis128x4_encrypt_vaes_avx512(const u8 key[AEGIS128X4_KEY_SIZE],
			       const u8 nonce[AEGIS128X4_NONCE_SIZE],
			       const u8 *ad, size_t ad_len, const u8 *src,
			       u8 *dst, size_t len, u8 *tag,
			       unsigned int authsize);
asmlinkage void
aegis128x4_decrypt_vaes_avx512(const u8 key[AEGIS128X4_KEY_SIZE],
			       const u8 nonce[AEGIS128X4_NONCE_SIZE],
			       const u8 *ad, size_t ad_len, const u8 *src,
			       u8 *dst, size_t len, u8 *tag,
			       unsigned int authsize);

static bool aegis128x4_simd_usable(void)
{
	return static_branch_likely(&have_vaes_avx512) && irq_fpu_usable();
}

static bool aegis128x4_init_arch(union aegis128x4_state *st,
				 const u8 key[AEGIS128X4_KEY_SIZE],
				 const u8 nonce[AEGIS128X4_NONCE_SIZE])
{
	if (!aegis128x4_simd_usable())
		return false;
	kernel_fpu_begin_mask(0);
	aegis128x4_init_vaes_avx512(st, key, nonce);
	kernel_fpu_end();
	return true;
}

static bool aegis128x4_absorb_arch(union aegis128x4_state *st, const u8 *src,
				   size_t nblocks)
{
	size_t len = nblocks * AEGIS128X4_BLOCK_SIZE;

	if (!len)
		return true;
	if (!aegis128x4_simd_usable())
		return false;
	do {
		size_t n = min_t(size_t, len, SZ_4K);

		kernel_fpu_begin_mask(0);
		aegis128x4_absorb_vaes_avx512(st, src, n);
		kernel_fpu_end();
		src += n;
		len -= n;
	} while (len);
	return true;
}

static bool aegis128x4_crypt_blocks_arch(union aegis128x4_state *st, u8 *dst,
					 const u8 *src, size_t nblocks,
					 bool enc)
{
	size_t len = nblocks * AEGIS128X4_BLOCK_SIZE;

	if (!len)
		return true;
	if (!aegis128x4_simd_usable())
		return false;
	do {
		size_t n = min_t(size_t, len, SZ_4K);

		kernel_fpu_begin_mask(0);
		if (enc)
			aegis128x4_enc_vaes_avx512(st, dst, src, n);
		else
			aegis128x4_dec_vaes_avx512(st, dst, src, n);
		kernel_fpu_end();
		src += n;
		dst += n;
		len -= n;
	} while (len);
	return true;
}

static bool aegis128x4_enc_arch(union aegis128x4_state *st, u8 *dst,
				const u8 *src, size_t nblocks)
{
	return aegis128x4_crypt_blocks_arch(st, dst, src, nblocks, true);
}

static bool aegis128x4_dec_arch(union aegis128x4_state *st, u8 *dst,
				const u8 *src, size_t nblocks)
{
	return aegis128x4_crypt_blocks_arch(st, dst, src, nblocks, false);
}

static bool aegis128x4_final_arch(union aegis128x4_state *st, u8 *authtag,
				  u64 ad_len, u64 data_len, size_t authtag_len)
{
	if (!aegis128x4_simd_usable())
		return false;
	kernel_fpu_begin_mask(0);
	aegis128x4_final_vaes_avx512(st, authtag, ad_len, data_len,
				     authtag_len);
	kernel_fpu_end();
	return true;
}

/*
 * Process a whole message in one call, keeping the state in registers, if it
 * is short enough for a single kernel-mode FPU section.  Longer messages go
 * through the incremental functions, which split them.
 */
static bool aegis128x4_crypt_arch(u8 *dst, const u8 *src, size_t data_len,
				  u8 *authtag, const u8 *ad, size_t ad_len,
				  const u8 nonce[AEGIS128X4_NONCE_SIZE],
				  const struct aegis128x4_key *key, bool enc)
{
	if (ad_len + data_len > SZ_4K || !aegis128x4_simd_usable())
		return false;
	kernel_fpu_begin_mask(0);
	if (enc)
		aegis128x4_encrypt_vaes_avx512(key->key, nonce, ad, ad_len, src,
					       dst, data_len, authtag,
					       key->authtag_len);
	else
		aegis128x4_decrypt_vaes_avx512(key->key, nonce, ad, ad_len, src,
					       dst, data_len, authtag,
					       key->authtag_len);
	kernel_fpu_end();
	return true;
}

#define aegis128x4_mod_init_arch aegis128x4_mod_init_arch
static void aegis128x4_mod_init_arch(void)
{
	/*
	 * There is no 256-bit code yet, so unlike AES-GCM this is also used on
	 * CPUs with X86_FEATURE_PREFER_YMM: the alternative is the much slower
	 * generic code.
	 */
	if (boot_cpu_has(X86_FEATURE_VAES) &&
	    boot_cpu_has(X86_FEATURE_AVX2) &&
	    boot_cpu_has(X86_FEATURE_AVX512BW) &&
	    boot_cpu_has(X86_FEATURE_BMI2))
		static_branch_enable(&have_vaes_avx512);
}
