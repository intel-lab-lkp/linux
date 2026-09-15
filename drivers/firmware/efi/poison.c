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

static bool __init
efi_poison_geometry_valid(const struct linux_efi_poisoned_memory *pm)
{
	u64 nbits, end;

	/* Whole words, and a bit count that can be taken without wrapping. */
	if (!pm->size || !IS_ALIGNED(pm->size, sizeof(unsigned long)) ||
	    check_mul_overflow(pm->size, (u64)BITS_PER_BYTE, &nbits))
		return false;

	/* And a footprint that can be page aligned without wrapping either. */
	if (check_add_overflow(efi.poisoned_memory, sizeof(*pm) + pm->size,
			       &end) || end > PHYS_ADDR_MAX - PAGE_SIZE)
		return false;

	if (pm->unit_size < PAGE_SIZE || !is_power_of_2(pm->unit_size))
		return false;

	return IS_ALIGNED(pm->phys_base, pm->unit_size);
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

	if (!efi_poison_geometry_valid(pm)) {
		pr_warn("Ignoring malformed poisoned-memory table\n");
		return false;
	}

	return true;
}

/*
 * Vet the inherited table and hand its pages to memblock, the way the
 * unaccepted memory table is handled. It is EFI ACPI reclaim memory, which
 * becomes E820_TYPE_ACPI and would otherwise stay out of the direct map, and
 * touching it then faults. Called from efi_config_parse_tables(), so
 * everything later can reach it with efi_poisoned_memory().
 */
void __init efi_poisoned_memory_reserve(void)
{
	struct linux_efi_poisoned_memory *pm;
	phys_addr_t start, end;

	if (efi.poisoned_memory == EFI_INVALID_TABLE_ADDR)
		return;

	pm = early_memremap(efi.poisoned_memory, sizeof(*pm));
	if (!pm) {
		pr_warn("Could not map poisoned-memory table\n");
		efi.poisoned_memory = EFI_INVALID_TABLE_ADDR;
		return;
	}

	if (!efi_poison_table_valid(pm)) {
		efi.poisoned_memory = EFI_INVALID_TABLE_ADDR;
		early_memunmap(pm, sizeof(*pm));
		return;
	}

	start = PAGE_ALIGN_DOWN(efi.poisoned_memory);
	end = PAGE_ALIGN(efi.poisoned_memory + sizeof(*pm) + pm->size);
	early_memunmap(pm, sizeof(*pm));

	memblock_add(start, end - start);
	memblock_reserve(start, end - start);
}

/* The table, vetted at parse time, or NULL if this boot has none. */
static struct linux_efi_poisoned_memory *efi_poisoned_memory(void)
{
	if (efi.poisoned_memory == EFI_INVALID_TABLE_ADDR)
		return NULL;

	return phys_to_virt(efi.poisoned_memory);
}

/*
 * A bit is never cleared: it stands for a whole EFI_POISON_UNIT_SIZE, so an
 * unpoison cannot tell whether the unit as a whole is good again.
 */
void efi_hwpoison_record_pfn(unsigned long pfn)
{
	struct linux_efi_poisoned_memory *pm = efi_poisoned_memory();
	phys_addr_t addr = PFN_PHYS(pfn);
	u64 unit;

	if (!pm || addr < pm->phys_base)
		return;

	unit = (addr - pm->phys_base) / pm->unit_size;
	if (unit < pm->size * BITS_PER_BYTE)
		set_bit(unit, pm->bitmap);
}
