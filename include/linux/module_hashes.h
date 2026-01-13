/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _LINUX_MODULE_HASHES_H
#define _LINUX_MODULE_HASHES_H

#include <linux/compiler_attributes.h>
#include <linux/types.h>
#include <crypto/sha2.h>

#define __module_hashes_section __section(".module_hashes")
#define MODULE_HASHES_HASH_SIZE SHA256_DIGEST_SIZE

struct module_hashes_proof {
	__be32 pos;
	u8 hash_sigs[][MODULE_HASHES_HASH_SIZE];
} __packed;

struct module_hashes_root {
	u32 levels;
	u8 hash[MODULE_HASHES_HASH_SIZE];
};

extern const struct module_hashes_root module_hashes_root;

#endif /* _LINUX_MODULE_HASHES_H */
