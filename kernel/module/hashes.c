// SPDX-License-Identifier: GPL-2.0-or-later
/* Module hash-based integrity checker
 *
 * Copyright (C) 2025 Thomas Weißschuh <linux@weissschuh.net>
 * Copyright (C) 2025 Sebastian Andrzej Siewior <sebastian@breakpoint.cc>
 *
 * The structure of the Merkle tree is documented in scripts/modules-merkle-tree.c.
 */

#define pr_fmt(fmt) "module/hash: " fmt

#include <linux/module_hashes.h>
#include <linux/module.h>
#include <linux/unaligned.h>

#include <crypto/sha2.h>

#include "internal.h"

static __init __maybe_unused int module_hashes_init(void)
{
	pr_debug("root: levels=%u hash=%*phN\n",
		 module_hashes_root.levels,
		 (int)sizeof(module_hashes_root.hash), &module_hashes_root.hash);

	return 0;
}

#if IS_ENABLED(CONFIG_MODULE_DEBUG)
early_initcall(module_hashes_init);
#endif

static void hash_entry(const struct module_hash *left, const struct module_hash *right,
		       struct module_hash *out)
{
	struct sha256_ctx ctx;
	u8 magic = 0x02;

	sha256_init(&ctx);
	sha256_update(&ctx, &magic, sizeof(magic));
	sha256_update(&ctx, left->h, sizeof(left->h));
	sha256_update(&ctx, right->h, sizeof(right->h));
	sha256_final(&ctx, out->h);
}

static void hash_data(const u8 *d, size_t len, unsigned int pos, struct module_hash *out)
{
	struct sha256_ctx ctx;
	u8 magic = 0x01;
	__be32 pos_be;

	pos_be = cpu_to_be32(pos);

	sha256_init(&ctx);
	sha256_update(&ctx, &magic, sizeof(magic));
	sha256_update(&ctx, (const u8 *)&pos_be, sizeof(pos_be));
	sha256_update(&ctx, d, len);
	sha256_final(&ctx, out->h);
}

static bool module_hashes_verify_proof(u32 pos, const struct module_hash *hash_sigs,
				       struct module_hash *cur)
{
	for (unsigned int i = 0; i < module_hashes_root.levels; i++, pos >>= 1) {
		if ((pos & 1) == 0)
			hash_entry(cur, &hash_sigs[i], cur);
		else
			hash_entry(&hash_sigs[i], cur, cur);
	}

	return !memcmp(cur, &module_hashes_root.hash, sizeof(module_hashes_root.hash));
}

int module_hash_check(const void *mod, size_t mod_len, const void *sig, size_t sig_len)
{
	const struct module_hashes_proof *proof;
	struct module_hash modhash;
	size_t proof_size;
	u32 pos;

	proof_size = struct_size(proof, hash_sigs, module_hashes_root.levels);

	if (sig_len != proof_size)
		return -ENOPKG;

	proof = (const struct module_hashes_proof *)sig;
	pos = get_unaligned_be32(&proof->pos);

	hash_data(mod, mod_len, pos, &modhash);

	if (!module_hashes_verify_proof(pos, proof->hash_sigs, &modhash))
		return -ENOKEY;

	return 0;
}
