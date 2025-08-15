// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/kpkeys.h>
#include <linux/set_memory.h>

DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);

int kpkeys_protect_pgtable_memory(struct folio *folio)
{
	unsigned long addr = (unsigned long)folio_address(folio);
	unsigned int order = folio_order(folio);
	int ret = 0;

	if (kpkeys_hardened_pgtables_enabled())
		ret = set_memory_pkey(addr, 1 << order, KPKEYS_PKEY_PGTABLES);

	WARN_ON(ret);
	return ret;
}

int kpkeys_unprotect_pgtable_memory(struct folio *folio)
{
	unsigned long addr = (unsigned long)folio_address(folio);
	unsigned int order = folio_order(folio);
	int ret = 0;

	if (kpkeys_hardened_pgtables_enabled())
		ret = set_memory_pkey(addr, 1 << order, KPKEYS_PKEY_DEFAULT);

	WARN_ON(ret);
	return ret;
}

void __init kpkeys_hardened_pgtables_enable(void)
{
	int ret;

	if (!arch_kpkeys_enabled())
		return;

	static_branch_enable(&kpkeys_hardened_pgtables_key);
	ret = kernel_pgtables_set_pkey(KPKEYS_PKEY_PGTABLES);
	WARN_ON(ret);
}
