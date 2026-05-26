// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kpkeys.h>
#include <linux/mm.h>
#include <linux/set_memory.h>

#include <kunit/visibility.h>

__ro_after_init DEFINE_STATIC_KEY_FALSE(kpkeys_hardened_pgtables_key);
EXPORT_SYMBOL_IF_KUNIT(kpkeys_hardened_pgtables_key);

static int set_pkey_pgtable(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_memory_pkey(addr, nr_pages, KPKEYS_PKEY_PGTABLES);

	WARN_ON(ret);
	return ret;
}

static int set_pkey_default(struct page *page, unsigned int nr_pages)
{
	unsigned long addr = (unsigned long)page_address(page);
	int ret;

	ret = set_memory_pkey(addr, nr_pages, KPKEYS_PKEY_DEFAULT);

	WARN_ON(ret);
	return ret;
}

struct page *kpkeys_pgtable_alloc(gfp_t gfp, unsigned int order)
{
	struct page *page;
	int ret;

	page = alloc_pages_noprof(gfp, order);
	if (!page)
		return page;

	ret = set_pkey_pgtable(page, 1 << order);
	if (ret) {
		__free_pages(page, order);
		return NULL;
	}

	return page;
}

void kpkeys_pgtable_free(struct page *page, unsigned int order)
{
	set_pkey_default(page, 1 << order);
	__free_pages(page, order);
}

void __init kpkeys_hardened_pgtables_init(void)
{
	if (!kpkeys_enabled())
		return;

	static_branch_enable(&kpkeys_hardened_pgtables_key);
}
