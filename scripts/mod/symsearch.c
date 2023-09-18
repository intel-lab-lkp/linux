// SPDX-License-Identifier: GPL-2.0

/* Helper functions for finding the symbol in an ELF which is "nearest"
 * to a given address.
 */

#include "modpost.h"

/* Struct used for binary search. */
struct syminfo {
	unsigned int symbol_index;
	unsigned int section_index;
	Elf_Addr addr;
};

/* Container used to hold an entire binary search table.
 * Entries in table are ascending, sorted first by section_index,
 * then by addr, and last by symbol_index.  The sorting by
 * symbol_index is used to duplicate the quirks of the prior
 * find_nearest_sym() function, where exact matches to an address
 * return the first symtab entry seen, but near misses return the
 * last symtab entry seen.
 * The first and last entries of the table are sentinels and their
 * values only matter in two places:  when we sort the table, and
 * on lookups, the end sentinel should not have an addr field which
 * matches its immediate predecessor.  To meet these requirements,
 * we initialize them to (0,0,0) and (max,max,max), and then after
 * sorting, we tweak the end sentinel's addr field accordingly.
 */
struct symsearch {
	size_t table_size;
	struct syminfo table[];
};

static inline bool is_sym_searchable(struct elf_info *elf, Elf_Sym *sym)
{
	return is_valid_name(elf, sym) != 0;
}

static int syminfo_compare(const void *s1, const void *s2)
{
	const struct syminfo *sym1 = s1;
	const struct syminfo *sym2 = s2;

	if (sym1->section_index > sym2->section_index)
		return 1;
	if (sym1->section_index < sym2->section_index)
		return -1;
	if (sym1->addr > sym2->addr)
		return 1;
	if (sym1->addr < sym2->addr)
		return -1;
	if (sym1->symbol_index > sym2->symbol_index)
		return 1;
	if (sym1->symbol_index < sym2->symbol_index)
		return -1;
	return 0;
}

static size_t symbol_count(struct elf_info *elf)
{
	size_t result = 0;

	for (Elf_Sym *sym = elf->symtab_start; sym < elf->symtab_stop; sym++) {
		if (is_sym_searchable(elf, sym))
			result++;
	}
	return result;
}

/* Populate the search array that we just allocated.
 * Be slightly paranoid here.  If the ELF file changes during processing,
 * or if the behavior of is_sym_searchable() changes during processing,
 * we want to catch it; neither of those is acceptable.
 */
static void symsearch_populate(struct elf_info *elf,
			       struct syminfo *table,
			       size_t table_size)
{
	bool is_arm = (elf->hdr->e_machine == EM_ARM);

	/* Start sentinel */
	if (table_size-- == 0)
		fatal("%s: size mismatch\n", __func__);
	table->symbol_index = 0;
	table->section_index = 0;
	table->addr = 0;
	table++;

	for (Elf_Sym *sym = elf->symtab_start; sym < elf->symtab_stop; sym++) {
		if (is_sym_searchable(elf, sym)) {
			if (table_size-- == 0)
				fatal("%s: size mismatch\n", __func__);
			table->symbol_index = sym - elf->symtab_start;
			table->section_index = get_secindex(elf, sym);
			table->addr = sym->st_value;

			/*
			 * For ARM Thumb instruction, the bit 0 of st_value is
			 * set if the symbol is STT_FUNC type. Mask it to get
			 * the address.
			 */
			if (is_arm && ELF_ST_TYPE(sym->st_info) == STT_FUNC)
				table->addr &= ~1;

			table++;
		}
	}

	/* End sentinel; all values are unsigned so -1 wraps to max */
	if (table_size != 1)
		fatal("%s: size mismatch\n", __func__);
	table->symbol_index = -1;
	table->section_index = -1;
	table->addr = -1;
}

void symsearch_init(struct elf_info *elf)
{
	/* +2 here to allocate space for the start and end sentinels */
	size_t table_size = symbol_count(elf) + 2;

	elf->symsearch = NOFAIL(malloc(
					sizeof(struct symsearch) +
					sizeof(struct syminfo) * table_size));
	elf->symsearch->table_size = table_size;

	symsearch_populate(elf, elf->symsearch->table, table_size);
	qsort(elf->symsearch->table, table_size,
	      sizeof(struct syminfo), syminfo_compare);

	/* A bit of paranoia; make sure that the end sentinel's address is
	 * different than its predecessor.  Not doing this could cause
	 * possible undefined behavior if anybody ever inserts a symbol
	 * with section_index and addr both at their max values.
	 * Doing this little bit of defensive programming is more efficient
	 * than checking for array overruns later.
	 */
	elf->symsearch->table[table_size - 1].addr =
		elf->symsearch->table[table_size - 2].addr + 1;
}

void symsearch_finish(struct elf_info *elf)
{
	free(elf->symsearch);
	elf->symsearch = NULL;
}

/* Find the syminfo which is in secndx and "nearest" to addr.
 * allow_negative: allow returning a symbol whose address is > addr.
 * min_distance: ignore symbols which are further away than this.
 *
 * Returns a nonzero index into the symsearch table for success.
 * Returns NULL if no legal symbol is found within the requested range.
 */
static size_t symsearch_find_impl(struct elf_info *elf, Elf_Addr addr,
				  unsigned int secndx, bool allow_negative,
				  Elf_Addr min_distance)
{
	/* Find the target in the array; it will lie between two elements.
	 * Invariant here: table[lo] < target <= table[hi]
	 * For the purposes of search, exact hits in the search array are
	 * considered greater than the target.	This means that if we do
	 * get an exact hit, then once the search terminates, table[hi]
	 * will be the exact match which has the lowest symbol index.
	 */
	struct syminfo *table = elf->symsearch->table;
	size_t hi = elf->symsearch->table_size - 1;
	size_t lo = 0;
	bool hi_is_usable = false;
	bool lo_is_usable = false;
	Elf_Addr hi_distance = -1;  // max Elf_Addr
	Elf_Addr lo_distance = -1;  // max Elf_Addr
	Elf_Addr min_distance_lo = min_distance;
	Elf_Addr min_distance_hi = allow_negative ? min_distance : 0;

	for (;;) {
		size_t mid;

		mid = lo + (hi - lo) / 2;
		if (mid == lo)
			break;
		if (secndx > table[mid].section_index) {
			lo = mid;
		} else if (secndx < table[mid].section_index) {
			hi = mid;
		} else if (addr > table[mid].addr) {
			lo = mid;
			lo_distance = addr - table[mid].addr;
			lo_is_usable = (lo_distance <= min_distance_lo);
		} else {
			hi = mid;
			hi_distance = table[mid].addr - addr;
			hi_is_usable = (hi_distance <= min_distance_hi);
		}
	}

	if (hi_is_usable && lo_is_usable) {
		lo_is_usable = (lo_distance <= hi_distance);
		hi_is_usable = (hi_distance <= lo_distance);
	}

	if (!hi_is_usable)
		return lo_is_usable ? lo : 0;

	if (hi_distance == 0)
		return hi;

	/* Match quirks of existing behavior.  Advance hi to the last
	 * matching entry in the search table.	We don't need to worry
	 * about running off the end of the array due to the sentinel.
	 */
	while (table[hi+1].addr == table[hi].addr &&
	       table[hi+1].section_index == table[hi].section_index) {
		hi++;
	}

	return (lo_is_usable &&
		table[lo].symbol_index > table[hi].symbol_index) ? lo : hi;
}

Elf_Sym *symsearch_find_nearest(struct elf_info *elf, Elf_Addr addr,
				unsigned int secndx, bool allow_negative,
				Elf_Addr min_distance)
{
	size_t result = symsearch_find_impl(elf, addr, secndx,
					    allow_negative, min_distance);

	if (result == 0)
		return NULL;

	return &elf->symtab_start[elf->symsearch->table[result].symbol_index];
}
