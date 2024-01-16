// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cryptographic API.
 *
 * Support for Zhaoxin hardware crypto engine.
 *
 * Copyright (c) 2023  George Xue <georgexue@zhaoxin.com>
 */

#include <crypto/internal/hash.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <asm/cpu_device_id.h>
#include <asm/fpu/api.h>
#include "zhaoxin-sha.h"

static inline void zhaoxin_output_block(uint32_t *src, uint32_t *dst, size_t count)
{
	while (count--)
		*dst++ = swab32(*src++);
}

static int zhaoxin_sha1_init(struct shash_desc *desc)
{
	struct sha1_state *sctx = shash_desc_ctx(desc);

	*sctx = (struct sha1_state){
		.state = { SHA1_H0, SHA1_H1, SHA1_H2, SHA1_H3, SHA1_H4 },
	};

	return 0;
}

static int zhaoxin_sha1_update(struct shash_desc *desc, const u8 *data, unsigned int len)
{
	struct sha1_state *sctx = shash_desc_ctx(desc);
	unsigned int partial, done;
	const u8 *src;
	u8 buf[SHA1_BLOCK_SIZE * 2];
	u8 *dst = &buf[0];

	partial = sctx->count & (SHA1_BLOCK_SIZE - 1);
	sctx->count += len;
	done = 0;
	src = data;
	memcpy(dst, sctx->state, SHA1_DIGEST_SIZE);

	if ((partial + len) >= SHA1_BLOCK_SIZE) {

		/* Append the bytes in state's buffer to a block to handle */
		if (partial) {
			done = -partial;
			memcpy(sctx->buffer + partial, data, done + SHA1_BLOCK_SIZE);
			src = sctx->buffer;

			asm volatile (".byte 0xf3,0x0f,0xa6,0xc8"
			: "+S"(src), "+D"(dst)
			: "a"(-1L), "c"(1UL));

			done += SHA1_BLOCK_SIZE;
			src = data + done;
		}

		/* Process the left bytes from the input data */
		if (len - done >= SHA1_BLOCK_SIZE) {
			asm volatile (".byte 0xf3,0x0f,0xa6,0xc8"
			: "+S"(src), "+D"(dst)
			: "a"(-1L),
			"c"((unsigned long)((len - done) / SHA1_BLOCK_SIZE)));

			done += ((len - done) - (len - done) % SHA1_BLOCK_SIZE);
			src = data + done;
		}
		partial = 0;
	}
	memcpy(sctx->state, dst, SHA1_DIGEST_SIZE);
	memcpy(sctx->buffer + partial, src, len - done);

	return 0;
}

static int zhaoxin_sha1_final(struct shash_desc *desc, u8 *out)
{
	struct sha1_state *state = shash_desc_ctx(desc);
	unsigned int partial, padlen;
	__be64 bits;
	static const u8 padding[SHA1_BLOCK_SIZE] = {SHA_PADDING_BYTE, };
	const int bit_offset = SHA1_BLOCK_SIZE - sizeof(__be64);

	bits = cpu_to_be64(state->count << 3);

	/* Padding */
	partial = state->count & (SHA1_BLOCK_SIZE - 1);
	padlen = (partial < bit_offset) ? (bit_offset - partial) :
		((SHA1_BLOCK_SIZE + bit_offset) - partial);
	zhaoxin_sha1_update(desc, padding, padlen);

	/* Append length field bytes */
	zhaoxin_sha1_update(desc, (const u8 *)&bits, sizeof(bits));

	/* Swap to output */
	zhaoxin_output_block(state->state, (uint32_t *)out, SHA1_DIGEST_SIZE/sizeof(uint32_t));

	return 0;
}

static int zhaoxin_sha256_init(struct shash_desc *desc)
{
	struct sha256_state *sctx = shash_desc_ctx(desc);

	*sctx = (struct sha256_state){
		.state = { SHA256_H0, SHA256_H1, SHA256_H2, SHA256_H3,
				SHA256_H4, SHA256_H5, SHA256_H6, SHA256_H7},
	};

	return 0;
}

static int zhaoxin_sha256_update(struct shash_desc *desc, const u8 *data,
			  unsigned int len)
{
	struct sha256_state *sctx = shash_desc_ctx(desc);
	unsigned int partial, done;
	const u8 *src;
	u8 buf[SHA256_BLOCK_SIZE*2];
	u8 *dst = &buf[0];

	partial = sctx->count & (SHA256_BLOCK_SIZE - 1);
	sctx->count += len;
	done = 0;
	src = data;
	memcpy(dst, sctx->state, SHA256_DIGEST_SIZE);

	if ((partial + len) >= SHA256_BLOCK_SIZE) {

		/* Append the bytes in state's buffer to a block to handle */
		if (partial) {
			done = -partial;
			memcpy(sctx->buf + partial, data, done + SHA256_BLOCK_SIZE);
			src = sctx->buf;

			asm volatile (".byte 0xf3,0x0f,0xa6,0xd0"
			: "+S"(src), "+D"(dst)
			: "a"(-1L), "c"(1UL));

			done += SHA256_BLOCK_SIZE;
			src = data + done;
		}

		/* Process the left bytes from input data*/
		if (len - done >= SHA256_BLOCK_SIZE) {
			asm volatile (".byte 0xf3,0x0f,0xa6,0xd0"
			: "+S"(src), "+D"(dst)
			: "a"(-1L),
			"c"((unsigned long)((len - done) / SHA256_BLOCK_SIZE)));

			done += ((len - done) - (len - done) % SHA256_BLOCK_SIZE);
			src = data + done;
		}
		partial = 0;
	}
	memcpy(sctx->state, dst, SHA256_DIGEST_SIZE);
	memcpy(sctx->buf + partial, src, len - done);

	return 0;
}

static int zhaoxin_sha256_final(struct shash_desc *desc, u8 *out)
{
	struct sha256_state *state = shash_desc_ctx(desc);
	unsigned int partial, padlen;
	__be64 bits;
	static const u8 padding[SHA256_BLOCK_SIZE] = {SHA_PADDING_BYTE, };
	const int bit_offset = SHA256_BLOCK_SIZE - sizeof(__be64);

	bits = cpu_to_be64(state->count << 3);

	/* Padding */
	partial = state->count & (SHA256_BLOCK_SIZE - 1);
	padlen = (partial < bit_offset) ? (bit_offset - partial) :
		((SHA256_BLOCK_SIZE + bit_offset) - partial);
	zhaoxin_sha256_update(desc, padding, padlen);

	/* Append length field bytes */
	zhaoxin_sha256_update(desc, (const u8 *)&bits, sizeof(bits));

	/* Swap to output */
	zhaoxin_output_block(state->state, (uint32_t *)out, SHA256_DIGEST_SIZE/sizeof(uint32_t));

	return 0;
}

static inline void zhaoxin_output_block_512(uint64_t *src,
			uint64_t *dst, size_t count)
{
	while (count--)
		*dst++ = swab64(*src++);
}

static int zhaoxin_sha384_init(struct shash_desc *desc)
{
	struct sha512_state *sctx = shash_desc_ctx(desc);

	*sctx = (struct sha512_state){
		.state = { SHA384_H0, SHA384_H1, SHA384_H2, SHA384_H3,
				SHA384_H4, SHA384_H5, SHA384_H6, SHA384_H7},
		.count = {0, 0},
	};

	return 0;
}

static int zhaoxin_sha512_init(struct shash_desc *desc)
{
	struct sha512_state *sctx = shash_desc_ctx(desc);

	*sctx = (struct sha512_state){
		.state = { SHA512_H0, SHA512_H1, SHA512_H2, SHA512_H3,
				SHA512_H4, SHA512_H5, SHA512_H6, SHA512_H7},
		.count = {0, 0},
	};

	return 0;
}

static int zhaoxin_sha512_update(struct shash_desc *desc, const u8 *data,
			  unsigned int len)
{
	struct sha512_state *sctx = shash_desc_ctx(desc);
	unsigned int partial, done;
	const u8 *src;
	u8 buf[SHA512_BLOCK_SIZE];
	u8 *dst = &buf[0];

	partial = sctx->count[0] % SHA512_BLOCK_SIZE;

	sctx->count[0] += len;
	if (sctx->count[0] < len)
		sctx->count[1]++;

	done = 0;
	src = data;
	memcpy(dst, sctx->state, SHA512_DIGEST_SIZE);

	if ((partial + len) >= SHA512_BLOCK_SIZE) {
		/* Append the bytes in state's buffer to a block to handle */
		if (partial) {

			done = -partial;
			memcpy(sctx->buf + partial, data, done + SHA512_BLOCK_SIZE);

			src = sctx->buf;

			asm volatile (".byte 0xf3,0x0f,0xa6,0xe0"
			: "+S"(src), "+D"(dst)
			: "c"(1UL));

			done += SHA512_BLOCK_SIZE;
			src = data + done;
		}

		/* Process the left bytes from input data*/
		if (len - done >= SHA512_BLOCK_SIZE) {
			asm volatile (".byte 0xf3,0x0f,0xa6,0xe0"
			: "+S"(src), "+D"(dst)
			: "c"((unsigned long)((len - done) / SHA512_BLOCK_SIZE)));

			done += ((len - done) - (len - done) % SHA512_BLOCK_SIZE);
			src = data + done;
		}
		partial = 0;
	}

	memcpy(sctx->state, dst, SHA512_DIGEST_SIZE);
	memcpy(sctx->buf + partial, src, len - done);

	return 0;
}

static int zhaoxin_sha512_final(struct shash_desc *desc, u8 *out)
{
	const int bit_offset = SHA512_BLOCK_SIZE - sizeof(__be64[2]);
	struct sha512_state *state = shash_desc_ctx(desc);
	unsigned int partial = state->count[0] % SHA512_BLOCK_SIZE, padlen;
	__be64 bits2[2];

	// Both SHA384 and SHA512 may be supported.
	int dgst_size = crypto_shash_digestsize(desc->tfm);

	static u8 padding[SHA512_BLOCK_SIZE];

	memset(padding, 0, SHA512_BLOCK_SIZE);
	padding[0] = SHA_PADDING_BYTE;

	// Convert byte count in little endian to bit count in big endian.
	bits2[0] = cpu_to_be64(state->count[1] << 3 | state->count[0] >> 61);
	bits2[1] = cpu_to_be64(state->count[0] << 3);

	padlen = (partial < bit_offset) ? (bit_offset - partial) :
		((SHA512_BLOCK_SIZE + bit_offset) - partial);

	zhaoxin_sha512_update(desc, padding, padlen);

	/* Append length field bytes */
	zhaoxin_sha512_update(desc, (const u8 *)bits2, sizeof(__be64[2]));

	/* Swap to output */
	zhaoxin_output_block_512(state->state, (uint64_t *)out, dgst_size/sizeof(uint64_t));

	return 0;
}

static int zhaoxin_sha_export(struct shash_desc *desc,
				void *out)
{
	int statesize = crypto_shash_statesize(desc->tfm);
	void *sctx = shash_desc_ctx(desc);

	memcpy(out, sctx, statesize);
	return 0;
}

static int zhaoxin_sha_import(struct shash_desc *desc,
				const void *in)
{
	int statesize = crypto_shash_statesize(desc->tfm);
	void *sctx = shash_desc_ctx(desc);

	memcpy(sctx, in, statesize);
	return 0;
}

static struct shash_alg sha1_alg = {
	.digestsize	=	SHA1_DIGEST_SIZE,
	.init		=	zhaoxin_sha1_init,
	.update		=	zhaoxin_sha1_update,
	.final		=	zhaoxin_sha1_final,
	.export		=	zhaoxin_sha_export,
	.import		=	zhaoxin_sha_import,
	.descsize	=	sizeof(struct sha1_state),
	.statesize	=	sizeof(struct sha1_state),
	.base		=	{
		.cra_name		=	"sha1",
		.cra_driver_name	=	"sha1-zhaoxin",
		.cra_priority		=	ZHAOXIN_SHA_CRA_PRIORITY,
		.cra_blocksize		=	SHA1_BLOCK_SIZE,
		.cra_module		=	THIS_MODULE,
	}
};

static struct shash_alg sha256_alg = {
	.digestsize	=	SHA256_DIGEST_SIZE,
	.init		=	zhaoxin_sha256_init,
	.update		=	zhaoxin_sha256_update,
	.final		=	zhaoxin_sha256_final,
	.export		=	zhaoxin_sha_export,
	.import		=	zhaoxin_sha_import,
	.descsize	=	sizeof(struct sha256_state),
	.statesize	=	sizeof(struct sha256_state),
	.base		=	{
		.cra_name		=	"sha256",
		.cra_driver_name	=	"sha256-zhaoxin",
		.cra_priority		=	ZHAOXIN_SHA_CRA_PRIORITY,
		.cra_blocksize		=	SHA256_BLOCK_SIZE,
		.cra_module		=	THIS_MODULE,
	}
};

static struct shash_alg sha384_alg = {
	.digestsize	=	SHA384_DIGEST_SIZE,
	.init		=	zhaoxin_sha384_init,
	.update		=	zhaoxin_sha512_update,
	.final		=	zhaoxin_sha512_final,
	.export		=	zhaoxin_sha_export,
	.import		=	zhaoxin_sha_import,
	.descsize	=	sizeof(struct sha512_state),
	.statesize	=	sizeof(struct sha512_state),
	.base		=	{
		.cra_name		=	"sha384",
		.cra_driver_name	=	"sha384-zhaoxin",
		.cra_priority		=	ZHAOXIN_SHA_CRA_PRIORITY,
		.cra_blocksize		=	SHA384_BLOCK_SIZE,
		.cra_module		=	THIS_MODULE,
	}
};

static struct shash_alg sha512_alg = {
	.digestsize	=	SHA512_DIGEST_SIZE,
	.init		=	zhaoxin_sha512_init,
	.update		=	zhaoxin_sha512_update,
	.final		=	zhaoxin_sha512_final,
	.export		=	zhaoxin_sha_export,
	.import		=	zhaoxin_sha_import,
	.descsize	=	sizeof(struct sha512_state),
	.statesize	=	sizeof(struct sha512_state),
	.base		=	{
		.cra_name		=	"sha512",
		.cra_driver_name	=	"sha512-zhaoxin",
		.cra_priority		=	ZHAOXIN_SHA_CRA_PRIORITY,
		.cra_blocksize		=	SHA512_BLOCK_SIZE,
		.cra_module		=	THIS_MODULE,
	}
};


static const struct x86_cpu_id zhaoxin_sha_ids[] = {
	X86_MATCH_VENDOR_FAM_FEATURE(ZHAOXIN, 6, X86_FEATURE_PHE, NULL),
	X86_MATCH_VENDOR_FAM_FEATURE(ZHAOXIN, 7, X86_FEATURE_PHE, NULL),
	X86_MATCH_VENDOR_FAM_FEATURE(CENTAUR, 7, X86_FEATURE_PHE, NULL),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, zhaoxin_sha_ids);

static int __init zhaoxin_sha_init(void)
{
	int rc = -ENODEV;

	struct shash_alg *sha1;
	struct shash_alg *sha256;
	struct shash_alg *sha384;
	struct shash_alg *sha512;

	if (!x86_match_cpu(zhaoxin_sha_ids) || !boot_cpu_has(X86_FEATURE_PHE_EN))
		return -ENODEV;

	sha1 = &sha1_alg;
	sha256 = &sha256_alg;

	rc = crypto_register_shash(sha1);
	if (rc)
		goto out;

	rc = crypto_register_shash(sha256);
	if (rc)
		goto out_unreg1;

	if (boot_cpu_has(X86_FEATURE_PHE2_EN)) {

		sha384 = &sha384_alg;
		sha512 = &sha512_alg;

		rc = crypto_register_shash(sha384);
		if (rc)
			goto out_unreg2;

		rc = crypto_register_shash(sha512);
		if (rc)
			goto out_unreg3;

		pr_notice("Using Zhaoxin Hardware Engine for SHA1/SHA256/SHA384/SHA512 algorithms.\n");
	} else
		pr_notice("Using Zhaoxin Hardware Engine for SHA1/SHA256 algorithms.\n");


	return 0;

out_unreg3:
	if (boot_cpu_has(X86_FEATURE_PHE2_EN))
		crypto_unregister_shash(sha384);

out_unreg2:
	crypto_unregister_shash(sha256);
out_unreg1:
	crypto_unregister_shash(sha1);

out:
	pr_err("Zhaoxin Hardware Engine for SHA1/SHA256/SHA384/SHA512 initialization failed.\n");
	return rc;
}

static void __exit zhaoxin_sha_fini(void)
{
	crypto_unregister_shash(&sha1_alg);
	crypto_unregister_shash(&sha256_alg);

	if (boot_cpu_has(X86_FEATURE_PHE2_EN)) {
		crypto_unregister_shash(&sha384_alg);
		crypto_unregister_shash(&sha512_alg);
	}

}

module_init(zhaoxin_sha_init);
module_exit(zhaoxin_sha_fini);

MODULE_DESCRIPTION("Zhaoxin Hardware SHA1/SHA256/SHA384/SHA512 algorithms support.");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("George Xue");

MODULE_ALIAS_CRYPTO("sha1-zhaoxin");
MODULE_ALIAS_CRYPTO("sha256-zhaoxin");
MODULE_ALIAS_CRYPTO("sha384-zhaoxin");
MODULE_ALIAS_CRYPTO("sha512-zhaoxin");
