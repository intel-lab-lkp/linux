/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _LINUX_MODULE_HASHES_H
#define _LINUX_MODULE_HASHES_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>
#include <crypto/sha2.h>

#define __module_hashes_section __section(".module_hashes")
#define MODULE_HASHES_HASH_SIZE SHA256_DIGEST_SIZE

struct module_hash {
	u8 h[MODULE_HASHES_HASH_SIZE];
};

struct module_hashes_proof {
	__be32 pos;
	struct module_hash hash_sigs[];
} __packed;

struct module_hashes_root {
	u32 levels;
	struct module_hash hash;
};

extern const struct module_hashes_root module_hashes_root;

#endif /* _LINUX_MODULE_HASHES_H */
