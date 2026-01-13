// SPDX-License-Identifier: GPL-2.0-or-later
/* Module hash-based integrity checker
 *
 * Copyright (C) 2025 Thomas Weißschuh <linux@weissschuh.net>
 * Copyright (C) 2025 Sebastian Andrzej Siewior <sebastian@breakpoint.cc>
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
		 (int)sizeof(module_hashes_root.hash), module_hashes_root.hash);

	return 0;
}

#if IS_ENABLED(CONFIG_MODULE_DEBUG)
early_initcall(module_hashes_init);
#endif

static void hash_entry(const void *left, const void *right, void *out)
{
	struct sha256_ctx ctx;
	u8 magic = 0x02;

	sha256_init(&ctx);
	sha256_update(&ctx, &magic, sizeof(magic));
	sha256_update(&ctx, left, MODULE_HASHES_HASH_SIZE);
	sha256_update(&ctx, right, MODULE_HASHES_HASH_SIZE);
	sha256_final(&ctx, out);
}

static void hash_data(const void *d, size_t len, unsigned int pos, void *out)
{
	struct sha256_ctx ctx;
	u8 magic = 0x01;
	__be32 pos_be;

	pos_be = cpu_to_be32(pos);

	sha256_init(&ctx);
	sha256_update(&ctx, &magic, sizeof(magic));
	sha256_update(&ctx, (const u8 *)&pos_be, sizeof(pos_be));
	sha256_update(&ctx, d, len);
	sha256_final(&ctx, out);
}

static bool module_hashes_verify_proof(u32 pos, const u8 hash_sigs[][MODULE_HASHES_HASH_SIZE],
				       u8 *cur)
{
	for (unsigned int i = 0; i < module_hashes_root.levels; i++, pos >>= 1) {
		if ((pos & 1) == 0)
			hash_entry(cur, hash_sigs[i], cur);
		else
			hash_entry(hash_sigs[i], cur, cur);
	}

	return !memcmp(cur, module_hashes_root.hash, MODULE_HASHES_HASH_SIZE);
}

int module_hash_check(struct load_info *info, const u8 *sig, size_t sig_len)
{
	u8 modhash[MODULE_HASHES_HASH_SIZE];
	const struct module_hashes_proof *proof;
	size_t proof_size;
	u32 pos;

	proof_size = struct_size(proof, hash_sigs, module_hashes_root.levels);

	if (sig_len != proof_size)
		return -ENOPKG;

	proof = (const struct module_hashes_proof *)sig;
	pos = get_unaligned_be32(&proof->pos);

	hash_data(info->hdr, info->len, pos, &modhash);

	if (module_hashes_verify_proof(pos, proof->hash_sigs, modhash))
		info->sig_ok = true;

	return 0;
}
