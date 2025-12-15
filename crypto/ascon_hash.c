// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Crypto API support for Ascon-Hash256
 * (https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-232.pdf)
 *
 * Copyright (C) Rusydi H. Makarim <rusydi.makarim@kriptograf.id>
 */

#include <crypto/internal/hash.h>
#include <crypto/ascon_hash.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define ASCON_HASH256_CTX(desc) ((struct ascon_hash256_ctx *)shash_desc_ctx(desc))

static int crypto_ascon_hash256_init(struct shash_desc *desc)
{
	ascon_hash256_init(ASCON_HASH256_CTX(desc));
	return 0;
}

static int crypto_ascon_hash256_update(struct shash_desc *desc, const u8 *data,
				       unsigned int len)
{
	ascon_hash256_update(ASCON_HASH256_CTX(desc), data, len);
	return 0;
}

static int crypto_ascon_hash256_final(struct shash_desc *desc, u8 *out)
{
	ascon_hash256_final(ASCON_HASH256_CTX(desc), out);
	return 0;
}

static int crypto_ascon_hash256_digest(struct shash_desc *desc, const u8 *data,
				       unsigned int len, u8 *out)
{
	ascon_hash256(data, len, out);
	return 0;
}

static int crypto_ascon_hash256_export_core(struct shash_desc *desc, void *out)
{
	memcpy(out, ASCON_HASH256_CTX(desc), sizeof(struct ascon_hash256_ctx));
	return 0;
}

static int crypto_ascon_hash256_import_core(struct shash_desc *desc,
					    const void *in)
{
	memcpy(ASCON_HASH256_CTX(desc), in, sizeof(struct ascon_hash256_ctx));
	return 0;
}

static struct shash_alg algs[] = { {
	.digestsize = ASCON_HASH256_DIGEST_SIZE,
	.init = crypto_ascon_hash256_init,
	.update = crypto_ascon_hash256_update,
	.final = crypto_ascon_hash256_final,
	.digest = crypto_ascon_hash256_digest,
	.export_core = crypto_ascon_hash256_export_core,
	.import_core = crypto_ascon_hash256_import_core,
	.descsize = sizeof(struct ascon_hash256_ctx),
	.base.cra_name = "ascon-hash256",
	.base.cra_driver_name = "ascon-hash256-lib",
	.base.cra_blocksize = ASCON_HASH256_BLOCK_SIZE,
	.base.cra_module = THIS_MODULE,
} };

static int __init crypto_ascon_hash256_mod_init(void)
{
	return crypto_register_shashes(algs, ARRAY_SIZE(algs));
}
module_init(crypto_ascon_hash256_mod_init);

static void __exit crypto_ascon_hash256_mod_exit(void)
{
	crypto_unregister_shashes(algs, ARRAY_SIZE(algs));
}
module_exit(crypto_ascon_hash256_mod_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Crypto API support for Ascon-Hash256");

MODULE_ALIAS_CRYPTO("ascon-hash256");
MODULE_ALIAS_CRYPTO("ascon-hash256-lib");
