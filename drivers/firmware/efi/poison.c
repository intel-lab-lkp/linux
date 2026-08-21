// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime handling for the LINUX_EFI_POISONED_MEMORY configuration table: a
 * bitmap with one bit per EFI_POISON_UNIT_SIZE of physical memory that records
 * hardware-poisoned frames so the next kexec kernel can keep them out of its
 * allocator. The stub allocates and installs the (empty) bitmap; this kernel
 * sets bits at runtime; the next kernel reserves the set units before the
 * allocator comes up.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Breno Leitao <leitao@debian.org>
 */

#define pr_fmt(fmt) "efi: " fmt

#include <linux/bitmap.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/memblock.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/overflow.h>

static struct linux_efi_poisoned_memory *efi_poison __ro_after_init;

/* A non-empty bitmap on a power-of-2 grid that phys_base actually sits on. */
static bool __init
efi_poison_geometry_valid(const struct linux_efi_poisoned_memory *pm)
{
	if (!pm->size)
		return false;
	if (pm->unit_size < PAGE_SIZE || !is_power_of_2(pm->unit_size))
		return false;

	return IS_ALIGNED(pm->phys_base, pm->unit_size);
}

/* The range the bitmap claims to describe has to fit in a u64. */
static bool __init
efi_poison_range_valid(const struct linux_efi_poisoned_memory *pm)
{
	u64 nbits, span;

	if (check_mul_overflow(pm->size, (u64)BITS_PER_BYTE, &nbits))
		return false;
	if (check_mul_overflow(nbits, (u64)pm->unit_size, &span))
		return false;

	return !check_add_overflow(pm->phys_base, span, &span);
}

/*
 * The table may have been installed by an earlier kernel in the kexec chain,
 * so check its geometry before doing any arithmetic with it.
 */
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

	/* Map the header to learn the bitmap size, then map the whole table. */
	pm = memremap(efi.poisoned_memory, sizeof(*pm), MEMREMAP_WB);
	if (WARN_ON_ONCE(!pm))
		return 0;
	if (!efi_poison_table_valid(pm)) {
		memunmap(pm);
		return 0;
	}
	size = pm->size;
	memunmap(pm);

	efi_poison = memremap(efi.poisoned_memory, sizeof(*pm) + size,
			      MEMREMAP_WB);
	WARN_ON_ONCE(!efi_poison);
	return 0;
}
early_initcall(efi_poison_init);

/* Bitmap unit covering @pfn, or -1 if the pfn falls outside the table. */
static long efi_poison_unit(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);
	u64 unit;

	if (addr < efi_poison->phys_base)
		return -1;
	unit = (addr - efi_poison->phys_base) / efi_poison->unit_size;
	if (unit >= (u64)efi_poison->size * BITS_PER_BYTE)
		return -1;
	return unit;
}

/*
 * Record a hardware-poisoned frame so the next kernel keeps its unit out of the
 * allocator. memory_failure() has already removed the frame from this kernel.
 *
 * A bit is never cleared: one bit stands for a whole EFI_POISON_UNIT_SIZE, so
 * an unpoison cannot tell whether the unit as a whole is good again.
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

void __init efi_reserve_poisoned_memory(void)
{
	u64 ppm = efi.poisoned_memory, phys_base, unit_size, bitmap_size, off;
	struct linux_efi_poisoned_memory *pm;
	unsigned int nr_units = 0;

	if (ppm == EFI_INVALID_TABLE_ADDR)
		return;

	pm = early_memremap(ppm, sizeof(*pm));
	if (!pm) {
		pr_warn("Could not map poisoned-memory table\n");
		return;
	}

	if (!efi_poison_table_valid(pm)) {
		/* Keep the runtime side off a table this pass rejected. */
		efi.poisoned_memory = EFI_INVALID_TABLE_ADDR;
		early_memunmap(pm, sizeof(*pm));
		return;
	}

	phys_base = pm->phys_base;
	unit_size = pm->unit_size;
	bitmap_size = pm->size;

	early_memunmap(pm, sizeof(*pm));

	/* Reserve the table itself so it survives a further kexec. */
	memblock_reserve(PAGE_ALIGN_DOWN(ppm),
			 PAGE_ALIGN(ppm + sizeof(*pm) + bitmap_size) -
			 PAGE_ALIGN_DOWN(ppm));

	/*
	 * Walk the bitmap a page at a time and reserve each poisoned unit.
	 * memblock.memory is not populated this early, so memblock_remove()
	 * and memblock_mark_nomap() would be no-ops; a reservation is what
	 * keeps the units away from the allocator.
	 */
	for (off = 0; off < bitmap_size; off += PAGE_SIZE) {
		u64 chunk = min_t(u64, PAGE_SIZE, bitmap_size - off);
		unsigned long bit, nbits = chunk * BITS_PER_BYTE;
		unsigned long *map;

		map = early_memremap(ppm + offsetof(struct linux_efi_poisoned_memory,
						    bitmap) + off, chunk);
		if (!map) {
			pr_warn("Could not map poisoned-memory bitmap\n");
			return;
		}
		for_each_set_bit(bit, map, nbits) {
			u64 unit = off * BITS_PER_BYTE + bit;

			memblock_reserve(phys_base + unit * unit_size, unit_size);
			nr_units++;
		}
		early_memunmap(map, chunk);
	}

	if (nr_units)
		pr_info("reserved %u poisoned unit(s) (%lluK each) inherited across kexec\n",
			nr_units, unit_size >> 10);
}
