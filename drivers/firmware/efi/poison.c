// SPDX-License-Identifier: GPL-2.0-only
/*
 * Runtime side of the LINUX_EFI_POISONED_MEMORY table: one bit per
 * EFI_POISON_UNIT_SIZE, set here as frames go bad, honored by the next kernel.
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
#include <linux/mm.h>
#include <linux/overflow.h>

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
