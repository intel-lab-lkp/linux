// SPDX-License-Identifier: GPL-2.0
/*
 * GHASH using the RISC-V Zbc/Zbkc (CLMUL) extension
 *
 * Copyright (C) 2023 VRULL GmbH
 * Author: Christoph Müllner <christoph.muellner@vrull.eu>
 *
 * Copyright (C) 2025 Siflower Communications Ltd
 * Author: Qingfang Deng <qingfang.deng@siflower.com.cn>
 */

#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/types.h>
#include <crypto/ghash.h>
#include <crypto/internal/hash.h>

#define GHASH_MOD_POLY	0xc200000000000000

struct riscv64_clmul_ghash_ctx {
	__uint128_t key;
};

struct riscv64_clmul_ghash_desc_ctx {
	__uint128_t shash;
	u8 buffer[GHASH_DIGEST_SIZE];
	int bytes;
};

static __always_inline u64 riscv_zbb_swab64(u64 val)
{
	asm (".option push\n"
	     ".option arch,+zbb\n"
	     "rev8 %0, %1\n"
	     ".option pop\n"
	     : "=r" (val) : "r" (val));
	return val;
}

static __always_inline __uint128_t get_unaligned_be128(const u8 *p)
{
	__uint128_t val;
#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
	val = *(__uint128_t *)p;
	val = riscv_zbb_swab64(val >> 64) | (__uint128_t)riscv_zbb_swab64(val) << 64;
#else
	val = (__uint128_t)p[0] << 120;
	val |= (__uint128_t)p[1] << 112;
	val |= (__uint128_t)p[2] << 104;
	val |= (__uint128_t)p[3] << 96;
	val |= (__uint128_t)p[4] << 88;
	val |= (__uint128_t)p[5] << 80;
	val |= (__uint128_t)p[6] << 72;
	val |= (__uint128_t)p[7] << 64;
	val |= (__uint128_t)p[8] << 56;
	val |= (__uint128_t)p[9] << 48;
	val |= (__uint128_t)p[10] << 40;
	val |= (__uint128_t)p[11] << 32;
	val |= (__uint128_t)p[12] << 24;
	val |= (__uint128_t)p[13] << 16;
	val |= (__uint128_t)p[14] << 8;
	val |= (__uint128_t)p[15];
#endif
	return val;
}

static __always_inline void put_unaligned_be128(__uint128_t val, u8 *p)
{
#ifdef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
	*(__uint128_t *)p = riscv_zbb_swab64(val >> 64) | (__uint128_t)riscv_zbb_swab64(val) << 64;
#else
	p[0] = val >> 120;
	p[1] = val >> 112;
	p[2] = val >> 104;
	p[3] = val >> 96;
	p[4] = val >> 88;
	p[5] = val >> 80;
	p[6] = val >> 72;
	p[7] = val >> 64;
	p[8] = val >> 56;
	p[9] = val >> 48;
	p[10] = val >> 40;
	p[11] = val >> 32;
	p[12] = val >> 24;
	p[13] = val >> 16;
	p[14] = val >> 8;
	p[15] = val;
#endif
}

static __always_inline __attribute_const__
__uint128_t clmul128(u64 a, u64 b)
{
	u64 hi, lo;

	asm(".option push\n"
	    ".option arch,+zbc\n"
	    "clmul	%0, %2, %3\n"
	    "clmulh	%1, %2, %3\n"
	    ".option pop\n"
	    : "=&r" (lo), "=&r" (hi) : "r" (a), "r" (b));
	return (__uint128_t)hi << 64 | lo;
}

static int riscv64_clmul_ghash_init(struct shash_desc *desc)
{
	struct riscv64_clmul_ghash_desc_ctx *dctx = shash_desc_ctx(desc);

	dctx->bytes = 0;
	dctx->shash = 0;
	return 0;
}

/* Compute GMULT (Xi*H mod f) using the Zbc (clmul) extensions.
 * Using the no-Karatsuba approach and clmul for the final reduction.
 * This results in an implementation with minimized number of instructions.
 * HW with clmul latencies higher than 2 cycles might observe a performance
 * improvement with Karatsuba. HW with clmul latencies higher than 6 cycles
 * might observe a performance improvement with additionally converting the
 * reduction to shift&xor. For a full discussion of this estimates see
 * https://github.com/riscv/riscv-crypto/blob/master/doc/supp/gcm-mode-cmul.adoc
 */
static void gcm_ghash_rv64i_zbc(__uint128_t *Xi, __uint128_t k, const u8 *inp, size_t len)
{
	u64 k_hi = k >> 64, k_lo = k, p_hi, p_lo;
	__uint128_t hash = *Xi, p;

	do {
		__uint128_t t0, t1, t2, t3, lo, mid, hi;

		/* Load the input data, byte-reverse them, and XOR them with Xi */
		p = get_unaligned_be128(inp);

		inp += GHASH_BLOCK_SIZE;
		len -= GHASH_BLOCK_SIZE;

		p ^= hash;
		p_hi = p >> 64;
		p_lo = p;

		/* Multiplication (without Karatsuba) */
		t0 = clmul128(p_lo, k_lo);
		t1 = clmul128(p_lo, k_hi);
		t2 = clmul128(p_hi, k_lo);
		t3 = clmul128(p_hi, k_hi);
		mid = t1 ^ t2;
		lo = t0 ^ (mid << 64);
		hi = t3 ^ (mid >> 64);

		/* Reduction with clmul */
		mid = clmul128(lo, GHASH_MOD_POLY);
		lo ^= mid << 64;
		hi ^= lo ^ (mid >> 64);
		hi ^= clmul128(lo >> 64, GHASH_MOD_POLY);
		hash = hi;
	} while (len);

	*Xi = hash;
}

static int riscv64_clmul_ghash_setkey(struct crypto_shash *tfm, const u8 *key, unsigned int keylen)
{
	struct riscv64_clmul_ghash_ctx *ctx = crypto_shash_ctx(tfm);
	__uint128_t k;

	if (keylen != GHASH_BLOCK_SIZE)
		return -EINVAL;

	k = get_unaligned_be128(key);
	k = (k << 1 | k >> 127) ^ (k >> 127 ? (__uint128_t)GHASH_MOD_POLY << 64 : 0);
	ctx->key = k;

	return 0;
}

static int riscv64_clmul_ghash_update(struct shash_desc *desc, const u8 *src, unsigned int srclen)
{
	struct riscv64_clmul_ghash_ctx *ctx = crypto_shash_ctx(desc->tfm);
	struct riscv64_clmul_ghash_desc_ctx *dctx = shash_desc_ctx(desc);
	unsigned int len;

	if (dctx->bytes) {
		if (dctx->bytes + srclen < GHASH_DIGEST_SIZE) {
			memcpy(dctx->buffer + dctx->bytes, src, srclen);
			dctx->bytes += srclen;
			return 0;
		}
		memcpy(dctx->buffer + dctx->bytes, src, GHASH_DIGEST_SIZE - dctx->bytes);

		gcm_ghash_rv64i_zbc(&dctx->shash, ctx->key, dctx->buffer, GHASH_DIGEST_SIZE);

		src += GHASH_DIGEST_SIZE - dctx->bytes;
		srclen -= GHASH_DIGEST_SIZE - dctx->bytes;
		dctx->bytes = 0;
	}

	len = round_down(srclen, GHASH_BLOCK_SIZE);
	if (len) {
		gcm_ghash_rv64i_zbc(&dctx->shash, ctx->key, src, len);
		src += len;
		srclen -= len;
	}

	if (srclen) {
		memcpy(dctx->buffer, src, srclen);
		dctx->bytes = srclen;
	}
	return 0;
}

static int riscv64_clmul_ghash_final(struct shash_desc *desc, u8 out[GHASH_DIGEST_SIZE])
{
	struct riscv64_clmul_ghash_ctx *ctx = crypto_shash_ctx(desc->tfm);
	struct riscv64_clmul_ghash_desc_ctx *dctx = shash_desc_ctx(desc);
	int i;

	if (dctx->bytes) {
		for (i = dctx->bytes; i < GHASH_DIGEST_SIZE; i++)
			dctx->buffer[i] = 0;
		gcm_ghash_rv64i_zbc(&dctx->shash, ctx->key, dctx->buffer, GHASH_DIGEST_SIZE);
		dctx->bytes = 0;
	}
	put_unaligned_be128(dctx->shash, out);
	return 0;
}

struct shash_alg riscv64_clmul_ghash_alg = {
	.init = riscv64_clmul_ghash_init,
	.update = riscv64_clmul_ghash_update,
	.final = riscv64_clmul_ghash_final,
	.setkey = riscv64_clmul_ghash_setkey,
	.descsize = sizeof(struct riscv64_clmul_ghash_desc_ctx),
	.digestsize = GHASH_DIGEST_SIZE,
	.base = {
		 .cra_blocksize = GHASH_BLOCK_SIZE,
		 .cra_ctxsize = sizeof(struct riscv64_clmul_ghash_ctx),
		 .cra_priority = 250,
		 .cra_name = "ghash",
		 .cra_driver_name = "ghash-riscv64-clmul",
		 .cra_module = THIS_MODULE,
	},
};

static int __init riscv64_clmul_ghash_mod_init(void)
{
	bool has_clmul, has_rev8;

	has_clmul = riscv_isa_extension_available(NULL, ZBC) ||
		    riscv_isa_extension_available(NULL, ZBKC);
	has_rev8 = riscv_isa_extension_available(NULL, ZBB) ||
		   riscv_isa_extension_available(NULL, ZBKB);
	if (has_clmul && has_rev8)
		return crypto_register_shash(&riscv64_clmul_ghash_alg);

	return -ENODEV;
}

static void __exit riscv64_clmul_ghash_mod_fini(void)
{
	crypto_unregister_shash(&riscv64_clmul_ghash_alg);
}

module_init(riscv64_clmul_ghash_mod_init);
module_exit(riscv64_clmul_ghash_mod_fini);

MODULE_DESCRIPTION("GHASH (RISC-V CLMUL accelerated)");
MODULE_AUTHOR("Qingfang Deng <dqfext@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("ghash");
