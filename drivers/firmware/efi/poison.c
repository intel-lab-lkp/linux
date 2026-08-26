// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime side of the LINUX_EFI_POISONED_MEMORY table: one bit per
 * EFI_POISON_UNIT_SIZE, set here as frames go bad, honored by the next kernel.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */

#define pr_fmt(fmt) "efi: " fmt

#include <linux/efi.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/overflow.h>

struct efi_poison_geometry {
	u64 table;		/* phys address of the table */
	u64 unit_size;
	u64 bitmap_size;
};

static struct linux_efi_poisoned_memory *efi_poison __ro_after_init;
static u64 efi_poison_nbits __ro_after_init;

static u64 __init
efi_poison_usable_size(const struct linux_efi_poisoned_memory *pm)
{
	u64 nr_units = DIV_ROUND_UP(PFN_PHYS(max_pfn), pm->unit_size);
	u64 bytes = DIV_ROUND_UP(nr_units, BITS_PER_BYTE);

	/* Whole words: the bitmap is reached an unsigned long at a time. */
	return min(round_up(bytes, sizeof(unsigned long)), pm->size);
}

static bool __init
efi_poison_geometry_valid(const struct linux_efi_poisoned_memory *pm)
{
	if (!pm->size || !IS_ALIGNED(pm->size, sizeof(unsigned long)))
		return false;

	return pm->unit_size >= PAGE_SIZE && is_power_of_2(pm->unit_size);
}

static bool __init
efi_poison_range_valid(const struct linux_efi_poisoned_memory *pm)
{
	u64 nbits, span;

	if (check_mul_overflow(pm->size, (u64)BITS_PER_BYTE, &nbits))
		return false;

	return !check_mul_overflow(nbits, (u64)pm->unit_size, &span);
}

/* The table may come from an earlier kernel, so vet it before using it. */
static bool __init
efi_poison_table_valid(const struct linux_efi_poisoned_memory *pm)
{
	if (pm->version != 1) {
		pr_warn("Ignoring poisoned-memory table with version %u\n",
			pm->version);
		return false;
	}

	if (!efi_poison_geometry_valid(pm) || !efi_poison_range_valid(pm)) {
		pr_warn("Ignoring malformed poisoned-memory table\n");
		return false;
	}

	return true;
}

static int __init efi_poison_init(void)
{
	struct linux_efi_poisoned_memory *pm;
	u64 size;

	if (efi.poisoned_memory == EFI_INVALID_TABLE_ADDR)
		return 0;

	pm = memremap(efi.poisoned_memory, sizeof(*pm), MEMREMAP_WB);
	if (WARN_ON_ONCE(!pm))
		return 0;
	if (!efi_poison_table_valid(pm)) {
		memunmap(pm);
		return 0;
	}
	size = efi_poison_usable_size(pm);
	memunmap(pm);
	if (!size)
		return 0;

	efi_poison = memremap(efi.poisoned_memory, sizeof(*pm) + size,
			      MEMREMAP_WB);
	if (WARN_ON_ONCE(!efi_poison))
		return 0;

	efi_poison_nbits = size * BITS_PER_BYTE;
	return 0;
}
early_initcall(efi_poison_init);

static long efi_poison_unit(unsigned long pfn)
{
	u64 unit = PFN_PHYS(pfn) / efi_poison->unit_size;

	if (unit >= efi_poison_nbits)
		return -1;
	return unit;
}

/*
 * A bit is never cleared: it stands for a whole EFI_POISON_UNIT_SIZE, so an
 * unpoison cannot tell whether the unit as a whole is good again.
 */
void efi_hwpoison_record_pfn(unsigned long pfn)
{
	long unit;

	if (!efi_poison)
		return;

	unit = efi_poison_unit(pfn);
	if (unit < 0)
		return;

	set_bit(unit, efi_poison->bitmap);
}

static bool __init efi_poison_read_geometry(u64 ppm,
					    struct efi_poison_geometry *g)
{
	struct linux_efi_poisoned_memory *pm;
	bool valid;

	pm = early_memremap(ppm, sizeof(*pm));
	if (!pm) {
		pr_warn("Could not map poisoned-memory table\n");
		return false;
	}

	valid = efi_poison_table_valid(pm);
	if (valid) {
		g->table = ppm;
		g->unit_size = pm->unit_size;
		g->bitmap_size = efi_poison_usable_size(pm);
	} else {
		/* Keep the runtime side off a table this pass rejected. */
		efi.poisoned_memory = EFI_INVALID_TABLE_ADDR;
	}

	early_memunmap(pm, sizeof(*pm));

	return valid;
}

static unsigned long __init
efi_poison_offline_unit(const struct efi_poison_geometry *g, u64 unit)
{
	unsigned long pfn = PHYS_PFN(unit * g->unit_size);
	unsigned long i, nr_pages = 0;

	for (i = 0; i < g->unit_size >> PAGE_SHIFT; i++)
		nr_pages += hwpoison_boot_pfn(pfn + i);

	return nr_pages;
}

static long __init efi_poison_walk_chunk(const struct efi_poison_geometry *g,
					 u64 off, unsigned long *nr_units)
{
	u64 chunk = min_t(u64, PAGE_SIZE, g->bitmap_size - off);
	unsigned long bit, nbits = chunk * BITS_PER_BYTE;
	unsigned long *map, nr_pages = 0;

	map = early_memremap(g->table + offsetof(struct linux_efi_poisoned_memory,
						 bitmap) + off, chunk);
	if (!map)
		return -1;

	for_each_set_bit(bit, map, nbits) {
		nr_pages += efi_poison_offline_unit(g, off * BITS_PER_BYTE + bit);
		(*nr_units)++;
	}

	early_memunmap(map, chunk);

	return nr_pages;
}

static long __init efi_poison_walk(const struct efi_poison_geometry *g,
				   unsigned long *nr_units)
{
	unsigned long nr_pages = 0;
	u64 off;

	for (off = 0; off < g->bitmap_size; off += PAGE_SIZE) {
		long nr = efi_poison_walk_chunk(g, off, nr_units);

		if (nr < 0) {
			pr_warn("Could not map poisoned-memory bitmap\n");
			return -1;
		}
		nr_pages += nr;
	}

	return nr_pages;
}

void __init efi_offline_poisoned_memory(void)
{
	struct efi_poison_geometry g;
	unsigned long nr_units = 0;
	long nr_pages, expected;

	if (efi.poisoned_memory == EFI_INVALID_TABLE_ADDR)
		return;

	if (!efi_poison_read_geometry(efi.poisoned_memory, &g))
		return;

	nr_pages = efi_poison_walk(&g, &nr_units);
	if (nr_pages < 0)
		return;

	if (nr_pages)
		pr_info("poisoned %ld page(s) inherited across kexec\n", nr_pages);

	expected = nr_units * (g.unit_size >> PAGE_SHIFT);
	if (nr_pages < expected)
		pr_warn("%ld inherited poisoned page(s) could not be taken out of use\n",
			expected - nr_pages);
}
