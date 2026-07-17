// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime handling for the LINUX_EFI_POISONED_MEMORY configuration table:
 * record and clear poisoned frames so the next kexec kernel can keep them out
 * of its allocator.
 *
 * Handling for the LINUX_EFI_POISONED_MEMORY configuration table: record and
 * clear poisoned frames at runtime, and reserve frames inherited across a kexec
 * before the allocator comes up, so the next kernel keeps them out.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */

#define pr_fmt(fmt) "efi: " fmt

#include <linux/efi.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>

static struct linux_efi_poisoned_memory *efi_poisoned_memory_root __ro_after_init;
static DEFINE_SPINLOCK(efi_poisoned_memory_lock);

static int __init efi_poisoned_memory_map_root(void)
{
	if (efi.poisoned_memory == EFI_INVALID_TABLE_ADDR)
		return -ENODEV;

	efi_poisoned_memory_root = memremap(efi.poisoned_memory,
					    sizeof(*efi_poisoned_memory_root),
					    MEMREMAP_WB);
	if (WARN_ON_ONCE(!efi_poisoned_memory_root))
		return -ENOMEM;
	return 0;
}

static int __init efi_poisoned_memory_root_init(void)
{
	if (efi_poisoned_memory_root)
		return 0;
	if (efi_poisoned_memory_map_root())
		efi_poisoned_memory_root = (void *)ULONG_MAX;
	return 0;
}
early_initcall(efi_poisoned_memory_root_init);

/*
 * Record a hardware-poisoned frame so the next kernel can keep it out of its
 * allocator. memory_failure() has already removed it from this kernel, so only
 * the cross-kexec record is needed here.
 */
void efi_hwpoison_record_pfn(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);
	struct linux_efi_poisoned_memory *pm;
	unsigned long ppm;
	int index;

	if (!efi_poisoned_memory_root ||
	    efi_poisoned_memory_root == (void *)ULONG_MAX)
		return;

	/* Try to claim a slot in an existing list entry. */
	for (ppm = efi_poisoned_memory_root->next; ppm; ) {
		pm = memremap(ppm, sizeof(*pm), MEMREMAP_WB);
		if (!pm)
			return;
		index = atomic_fetch_add_unless(&pm->count, 1, pm->size);
		if (index < pm->size) {
			pm->entry[index].base = addr;
			pm->entry[index].size = PAGE_SIZE;
			memunmap(pm);
			return;
		}
		ppm = pm->next;
		memunmap(pm);
	}

	/*
	 * No slot free - allocate a new list entry and link it in. The page
	 * stays allocated for the rest of this boot, and the next kernel
	 * reserves it while parsing the EFI configuration tables, so no
	 * separate cross-kexec reservation is needed here.
	 */
	pm = (void *)__get_free_page(GFP_ATOMIC);
	if (!pm)
		return;

	pm->size = EFI_POISONED_MEMORY_COUNT(SZ_4K);
	atomic_set(&pm->count, 1);
	pm->entry[0].base = addr;
	pm->entry[0].size = PAGE_SIZE;

	spin_lock(&efi_poisoned_memory_lock);
	pm->next = efi_poisoned_memory_root->next;
	efi_poisoned_memory_root->next = __pa(pm);
	spin_unlock(&efi_poisoned_memory_lock);
}

/*
 * A frame was unpoisoned (typically the hwpoison injector under test).
 * Tombstone its entry so the next kernel does not reserve a now-good frame.
 */
void efi_hwpoison_unrecord_pfn(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);
	struct linux_efi_poisoned_memory *pm;
	unsigned long ppm;
	int i;

	if (!efi_poisoned_memory_root ||
	    efi_poisoned_memory_root == (void *)ULONG_MAX)
		return;

	for (ppm = efi_poisoned_memory_root->next; ppm; ) {
		pm = memremap(ppm, sizeof(*pm), MEMREMAP_WB);
		if (!pm)
			return;
		for (i = 0; i < atomic_read(&pm->count); i++) {
			if (pm->entry[i].base == addr) {
				pm->entry[i].size = 0;
				memunmap(pm);
				return;
			}
		}
		ppm = pm->next;
		memunmap(pm);
	}
}

/*
 * Reserve every frame recorded in the poisoned-memory table inherited across
 * kexec, before the page allocator is up. Called from efi_config_parse_tables().
 */
int __init efi_reserve_poisoned_memory(void)
{
	unsigned long ppm = efi.poisoned_memory;
	unsigned int nr_poison = 0;
	int i;

	if (ppm == EFI_INVALID_TABLE_ADDR)
		return 0;

	while (ppm) {
		struct linux_efi_poisoned_memory *pm;
		u8 *p;

		p = early_memremap(ALIGN_DOWN(ppm, PAGE_SIZE), PAGE_SIZE);
		if (!p) {
			pr_err("Could not map poisoned-memory entry!\n");
			return -ENOMEM;
		}

		pm = (void *)(p + ppm % PAGE_SIZE);

		/* reserve the list entry itself */
		memblock_reserve(ppm, struct_size(pm, entry, pm->size));

		for (i = 0; i < atomic_read(&pm->count); i++) {
			/* skip entries cleared by a later unpoison */
			if (pm->entry[i].size) {
				memblock_reserve(pm->entry[i].base,
						 pm->entry[i].size);
				nr_poison++;
			}
		}

		ppm = pm->next;
		early_memunmap(p, PAGE_SIZE);
	}

	if (nr_poison)
		pr_info("reserved %u hardware-poisoned frame(s) inherited across kexec\n",
			nr_poison);
	return 0;
}
