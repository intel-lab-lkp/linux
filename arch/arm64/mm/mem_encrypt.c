// SPDX-License-Identifier: GPL-2.0-only
/*
 * Implementation of the memory encryption/decryption API.
 *
 * Since the low-level details of the operation depend on the
 * Confidential Computing environment (e.g. pKVM, CCA, ...), this just
 * acts as a top-level dispatcher to whatever hooks may have been
 * registered.
 *
 * Author: Will Deacon <will@kernel.org>
 * Copyright (C) 2024 Google LLC
 *
 * "Hello, boils and ghouls!"
 */

#include <linux/bug.h>
#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/mem_encrypt.h>

static const struct arm64_mem_crypt_ops *crypt_ops;

int arm64_mem_crypt_ops_register(const struct arm64_mem_crypt_ops *ops)
{
	if (WARN_ON(crypt_ops))
		return -EBUSY;

	crypt_ops = ops;
	return 0;
}

int set_memory_encrypted(unsigned long addr, int numpages)
{
	if (likely(!crypt_ops))
		return 0;

	if (WARN_ON(!IS_ALIGNED(addr, mem_decrypt_granule_size())))
		return -EINVAL;

	if (WARN_ON(!IS_ALIGNED(numpages << PAGE_SHIFT, mem_decrypt_granule_size())))
		return -EINVAL;

	return crypt_ops->encrypt(addr, numpages);
}
EXPORT_SYMBOL_GPL(set_memory_encrypted);

int set_memory_decrypted(unsigned long addr, int numpages)
{
	if (likely(!crypt_ops))
		return 0;

	if (WARN_ON(!IS_ALIGNED(addr, mem_decrypt_granule_size())))
		return -EINVAL;

	if (WARN_ON(!IS_ALIGNED(numpages << PAGE_SHIFT, mem_decrypt_granule_size())))
		return -EINVAL;

	return crypt_ops->decrypt(addr, numpages);
}
EXPORT_SYMBOL_GPL(set_memory_decrypted);

size_t mem_decrypt_granule_size(void)
{
	if (is_realm_world())
		return max(PAGE_SIZE, realm_get_hyp_pagesize());
	return PAGE_SIZE;
}
EXPORT_SYMBOL_GPL(mem_decrypt_granule_size);
