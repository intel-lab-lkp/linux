// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel side of the LINUX_EFI_POISONED_MEMORY table: one bit per
 * EFI_POISON_UNIT_SIZE, set as frames go bad, and turned into memblock
 * reservations by the next kernel.
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
	const u64 max_end = PHYS_ADDR_MAX - PAGE_SIZE;
	u64 nbits, span, top, end, len;

	/* Whole words, and a bit count that can be taken without wrapping. */
	if (!pm->size || !IS_ALIGNED(pm->size, sizeof(unsigned long)) ||
	    check_mul_overflow(pm->size, (u64)BITS_PER_BYTE, &nbits))
		return false;

	/* And a footprint that can be page aligned without wrapping either. */
	len = sizeof(*pm) + pm->size;
	if (check_add_overflow(efi.poisoned_memory, len, &end) ||
	    end > max_end)
		return false;

	if (pm->unit_size < PAGE_SIZE || !is_power_of_2(pm->unit_size))
		return false;

	/* Every unit has to lie inside the address space. */
	if (check_mul_overflow(nbits, (u64)pm->unit_size, &span) ||
	    check_add_overflow(pm->phys_base, span, &top))
		return false;

	return IS_ALIGNED(pm->phys_base, pm->unit_size);
}

/*
 * The table came from an earlier kernel, check version and geometry,
 * otherwise bail out.
 */
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
 * Reserve every unit the table records, so that memblock allocates nothing
 * from it and memmap_init_reserved_pages() flags its frames. The bitmap is
 * read through early_memremap(): the table is not in the direct map yet.
 */
static void __init
efi_poison_reserve_units(phys_addr_t table,
			 const struct linux_efi_poisoned_memory *hdr)
{
	const phys_addr_t bitmap = table +
		offsetof(struct linux_efi_poisoned_memory, bitmap);
	const u32 unit_size = hdr->unit_size;
	unsigned int nr_units = 0, nr_overlap = 0, nr_skipped = 0;
	u64 off;

	for (off = 0; off < hdr->size; off += PAGE_SIZE) {
		u64 chunk = min_t(u64, PAGE_SIZE, hdr->size - off);
		u64 nbits = chunk * BITS_PER_BYTE;
		unsigned long bit, *map;

		map = early_memremap(bitmap + off, chunk);
		if (!map) {
			pr_warn("Could not map poisoned-memory bitmap\n");
			break;
		}

		for_each_set_bit(bit, map, nbits) {
			u64 unit = off * BITS_PER_BYTE + bit;
			phys_addr_t base = hdr->phys_base + unit * unit_size;
			bool overlap;

			overlap = memblock_is_region_reserved(base, unit_size);
			if (memblock_reserve_hwpoison(base, unit_size)) {
				nr_skipped++;
			} else {
				nr_units++;
				if (overlap)
					nr_overlap++;
			}
		}
		early_memunmap(map, chunk);
	}

	if (nr_units)
		pr_info("reserved %u poisoned unit(s) of %uK inherited across kexec\n",
			nr_units, unit_size >> 10);
	if (nr_overlap)
		pr_info("%u poisoned unit(s) overlap an earlier reservation, only their free frames are flagged\n",
			nr_overlap);
	if (nr_skipped)
		pr_warn("could not reserve %u poisoned unit(s)\n", nr_skipped);
}

/* The vetted header of the inherited table. */
static struct linux_efi_poisoned_memory efi_poison_hdr __initdata;

/*
 * Vet the inherited table and reserve its pages. Called from
 * efi_config_parse_tables(); the units it records wait for the architecture,
 * see efi_poisoned_memory_reserve_units().
 */
void __init efi_poisoned_memory_reserve(void)
{
	struct linux_efi_poisoned_memory *pm;
	phys_addr_t table = efi.poisoned_memory;
	phys_addr_t start, end;
	u64 len;

	if (table == EFI_INVALID_TABLE_ADDR)
		return;

	pm = early_memremap(table, sizeof(*pm));
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

	efi_poison_hdr = *pm;
	early_memunmap(pm, sizeof(*pm));

	len = sizeof(efi_poison_hdr) + efi_poison_hdr.size;
	start = PAGE_ALIGN_DOWN(table);
	end = PAGE_ALIGN(table + len);
	memblock_reserve(start, end - start);
}

/*
 * Reserve the units the table records. The architecture calls this once the
 * kernel image, the initrd and its other early reservations are in memblock,
 * so that a unit sharing frames with them leaves those frames alone.
 */
void __init efi_poisoned_memory_reserve_units(void)
{
	if (efi.poisoned_memory == EFI_INVALID_TABLE_ADDR)
		return;

	efi_poison_reserve_units(efi.poisoned_memory, &efi_poison_hdr);
}
