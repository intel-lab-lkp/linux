// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/init.h>
#include <linux/kernel.h>

#include <asm/cpuid.h>
#include <asm/cpuid/internal_api.h>
#include <asm/percpu.h>
#include <asm/processor.h>

#include "cpuid_scanner.h"

/**
 * __define_cpuid_read_function() - Generate a CPUID scanner read function
 * @_suffix:	Suffix for the generated function name (full name: cpuid_read_@_suffix())
 * @_leaf_t:	Type to cast the CPUID query output storage pointer
 * @_leaf:	Name of the CPUID query storage pointer
 * @_break_c:	Condition to break the CPUID scanning loop, which may reference @_leaf,
 *		and where @_leaf stores each iteration's CPUID query output.
 *
 * Define a CPUID scanner read function according to the requirements stated
 * at &struct cpuid_scan_entry->read().
 */
#define __define_cpuid_read_function(_suffix, _leaf_t, _leaf, _break_c)				\
static void											\
cpuid_read_##_suffix(const struct cpuid_scan_entry *e, struct cpuid_read_output *output)	\
{												\
	struct _leaf_t *_leaf = (struct _leaf_t *)output->leaf;					\
												\
	static_assert(sizeof(*_leaf) == 16);							\
												\
	output->info->nr_entries = 0;								\
	for (int i = 0; i < e->maxcnt; i++, _leaf++, output->info->nr_entries++) {		\
		cpuid_subleaf(e->leaf, e->subleaf + i, _leaf);					\
		if (_break_c)									\
			break;									\
	}											\
}

/*
 * Default CPUID scanner read function
 */
__define_cpuid_read_function(generic, cpuid_regs, ignored, false);

/*
 * Shared read function for Intel CPUID leaf 0x4 and AMD CPUID leaf 0x8000001d,
 * as both have the same subleaf enumeration logic and registers output format.
 */
__define_cpuid_read_function(deterministic_cache, leaf_0x4_0, leaf, leaf->cache_type == 0);

static void cpuid_read_0x2(const struct cpuid_scan_entry *e, struct cpuid_read_output *output)
{
	union leaf_0x2_regs *regs = (union leaf_0x2_regs *)output->leaf;
	struct leaf_0x2_0 *l2 = (struct leaf_0x2_0 *)output->leaf;
	int invalid_regs = 0;

	/*
	 * All Intel CPUs must report an iteration count of 1.	In case of
	 * bogus hardware, keep the leaf marked as invalid at the CPUID table.
	 */
	cpuid_subleaf(e->leaf, e->subleaf, l2);
	if (l2->iteration_count != 0x01)
		return;

	/*
	 * The most significant bit (MSB) of each register must be clear.
	 * If a register is malformed, replace its descriptors with NULL.
	 */
	for (int i = 0; i < 4; i++) {
		if (regs->reg[i].invalid) {
			regs->regv[i] = 0;
			invalid_regs++;
		}
	}

	/*
	 * If all the output registers were malformed, keep the leaf marked
	 * as invalid at the CPUID table.
	 */
	if (invalid_regs == 4)
		return;

	output->info->nr_entries = 1;
}

static void cpuid_read_0x80000000(const struct cpuid_scan_entry *e, struct cpuid_read_output *output)
{
	struct leaf_0x80000000_0 *el0 = (struct leaf_0x80000000_0 *)output->leaf;

	cpuid_subleaf(e->leaf, e->subleaf, el0);

	/*
	 * Protect against 32-bit CPUs lacking extended CPUID support: Max
	 * extended CPUID leaf must be in the 0x80000001-0x8000ffff range.
	 */
	if ((el0->max_ext_leaf & 0xffff0000) != 0x80000000) {
		*el0 = (struct leaf_0x80000000_0){ };
		return;
	}

	output->info->nr_entries = 1;
}

static unsigned int cpuid_range_max_leaf(const struct cpuid_leaves *leaves, unsigned int range)
{
	switch (range) {
	case CPUID_BASE_START:	return cpuid_get(leaves, 0x0).max_std_leaf;
	case CPUID_EXT_START:   return cpuid_get(leaves, 0x80000000).max_ext_leaf;
	default:		return 0;
	}
}

static bool
cpuid_range_valid(const struct cpuid_leaves *l, unsigned int leaf, unsigned int start, unsigned int end)
{
	if (leaf < start || leaf > end)
		return false;

	return leaf == start || leaf <= cpuid_range_max_leaf(l, start);
}

static bool cpuid_leaf_valid(const struct cpuid_leaves *l, unsigned int leaf)
{
	return	cpuid_range_valid(l, leaf, CPUID_BASE_START, CPUID_BASE_END) ||
		cpuid_range_valid(l, leaf, CPUID_EXT_START, CPUID_EXT_END);
}

const struct cpuid_scan_entry cpuid_common_scan_entries[] = {
	CPUID_SCAN_ENTRIES
};

const int cpuid_common_scan_entries_size = ARRAY_SIZE(cpuid_common_scan_entries);

static void cpuid_scan(const struct cpuid_scan_info *info)
{
	const struct cpuid_scan_entry *entry = info->entries;
	struct cpuid_leaves *leaves = &info->cpuid_table->leaves;

	for (unsigned int i = 0; i < info->nr_entries; i++, entry++) {
		struct cpuid_read_output output = {
			.leaf		= cpuid_leaves_leaf_p(leaves, entry->leaf_offs),
			.info		= cpuid_leaves_info_p(leaves, entry->info_offs),
		};

		if (!cpuid_leaf_valid(&info->cpuid_table->leaves, entry->leaf))
			continue;

		entry->read(entry, &output);
	}
}

/**
 * cpuid_scan_cpu() - Populate CPUID data for the current CPU
 * @c:		CPU capability structure associated with the current CPU
 *
 * Populate the CPUID table embedded within @c with scanned CPUID data.
 * Since all the CPUID instructions are run locally, this function must be
 * called on the CPU associated with @c.
 */
void cpuid_scan_cpu(struct cpuinfo_x86 *c)
{
	const struct cpuid_scan_info info = {
		.cpuid_table	= &c->cpuid_table,
		.entries	= cpuid_common_scan_entries,
		.nr_entries	= cpuid_common_scan_entries_size,
	};

	cpuid_scan(&info);
}
