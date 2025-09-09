// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Amazon.com Inc. or its affiliates.
 * Pratyush Yadav <pratyush@kernel.org>
 */

#include <linux/kexec_handover.h>
#include <linux/kho_array.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/bug.h>
#include <linux/types.h>

#define KA_PAGE_NR_ENTRIES ((PAGE_SIZE - sizeof(struct kho_array_page)) / sizeof(u64))

#define KA_ITER_PAGEPOS(iter) ((iter)->pos - (iter)->cur->startpos)
#define KA_PAGE(phys) ((phys) ? (struct kho_array_page *)__va((phys)) : NULL)

bool ka_iter_end(struct ka_iter *iter)
{
	return !iter->cur || (KA_ITER_PAGEPOS(iter) >= KA_PAGE_NR_ENTRIES && !iter->cur->next);
}

void *ka_iter_getentry(struct ka_iter *iter)
{
	if (!iter->cur || KA_ITER_PAGEPOS(iter) >= KA_PAGE_NR_ENTRIES)
		return NULL;

	return (void *)iter->cur->entries[KA_ITER_PAGEPOS(iter)];
}

static int ka_iter_extend(struct ka_iter *iter)
{
	struct kho_array_page *kap;
	struct folio *folio;
	u64 phys;

	if (!ka_iter_end(iter))
		return 0;

	folio = folio_alloc(GFP_KERNEL | __GFP_ZERO, 0);
	if (!folio)
		return -ENOMEM;

	kap = folio_address(folio);
	kap->startpos = rounddown(iter->pos, KA_PAGE_NR_ENTRIES);

	phys = (u64)PFN_PHYS(folio_pfn(folio));
	/*
	 * If the iterator already has a page, insert the page after it.
	 * Otherwise, set the page as the first in the array.
	 */
	if (iter->cur)
		iter->cur->next = phys;
	else
		iter->ka->first = phys;

	iter->cur = kap;

	return 0;
}

void ka_iter_init_read(struct ka_iter *iter, struct kho_array *ka)
{
	memset(iter, 0, sizeof(*iter));
	iter->ka = ka;
	iter->mode = KA_ITER_READ;
	iter->cur = KA_PAGE(ka->first);

	/* Make the iterator point to first valid entry. */
	if (!ka_iter_getentry(iter))
		ka_iter_nextentry(iter);
}

void ka_iter_init_write(struct ka_iter *iter, struct kho_array *ka)
{
	kho_array_init(ka);
	memset(iter, 0, sizeof(*iter));
	iter->ka = ka;
	iter->mode = KA_ITER_WRITE;
}

int ka_iter_init_restore(struct ka_iter *iter, struct kho_array *ka)
{
	int err;

	err = kho_array_restore(ka);
	if (err)
		return err;

	ka_iter_init_read(iter, ka);
	return 0;
}

int ka_iter_setpos(struct ka_iter *iter, unsigned long pos)
{
	if (pos < iter->pos)
		return -EINVAL;

	iter->pos = pos;

	/*
	 * The iterator must point to the highest page with startpos <= pos.
	 * Advance it as far as possible.
	 */
	while (iter->cur && KA_PAGE(iter->cur->next) &&
	       KA_PAGE(iter->cur->next)->startpos <= pos)
		iter->cur = KA_PAGE(iter->cur->next);

	return 0;
}

int ka_iter_setentry(struct ka_iter *iter, const void *value)
{
	int err = 0;

	if (iter->mode != KA_ITER_WRITE)
		return -EPERM;

	err = ka_iter_extend(iter);
	if (err)
		return err;

	iter->cur->entries[KA_ITER_PAGEPOS(iter)] = (u64)value;
	return 0;
}

void *ka_iter_nextentry(struct ka_iter *iter)
{
	ka_iter_setpos(iter, iter->pos + 1);
	while (!ka_iter_end(iter) && !ka_iter_getentry(iter)) {
		/*
		 * If we are in the hole between two pages, jump to the next
		 * page.
		 */
		if (KA_ITER_PAGEPOS(iter) >= KA_PAGE_NR_ENTRIES)
			/*
			 * The check for ka_iter_end() above makes sure next
			 * page exists.
			 *
			 * TODO: This is a bit nasty and might attract review
			 * comments. Can I make it cleaner?
			 */
			ka_iter_setpos(iter, KA_PAGE(iter->cur->next)->startpos);
		else
			ka_iter_setpos(iter, iter->pos + 1);
	}

	return ka_iter_getentry(iter);
}

bool kho_array_valid(struct kho_array *ka)
{
	return ka->magic == KHO_ARRAY_MAGIC && ka->version == KHO_ARRAY_VERSION;
}

void kho_array_init(struct kho_array *ka)
{
	memset(ka, 0, sizeof(*ka));
	ka->magic = KHO_ARRAY_MAGIC;
	ka->version = KHO_ARRAY_VERSION;
}

void kho_array_destroy(struct kho_array *ka)
{
	u64 cur = ka->first, next;

	while (cur) {
		next = KA_PAGE(cur)->next;
		folio_put(pfn_folio(PHYS_PFN(cur)));
		cur = next;
	}

	ka->magic = 0;
}

int kho_array_preserve(struct kho_array *ka)
{
	u64 cur = ka->first;
	int err;

	while (cur) {
		err = kho_preserve_folio(pfn_folio(PHYS_PFN(cur)));
		if (err)
			return err;

		cur = KA_PAGE(cur)->next;
	}

	return 0;
}

int kho_array_restore(struct kho_array *ka)
{
	u64 cur = ka->first;
	struct folio *folio;

	if (!kho_array_valid(ka))
		return -EOPNOTSUPP;

	while (cur) {
		folio = kho_restore_folio(cur);
		if (!folio)
			return -ENOMEM;
		cur = KA_PAGE(cur)->next;
	}

	return 0;
}
