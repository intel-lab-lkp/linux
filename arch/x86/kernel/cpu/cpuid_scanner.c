// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/init.h>
#include <linux/kernel.h>

#include <asm/cpuid.h>
#include <asm/cpuid/internal_api.h>
#include <asm/percpu.h>
#include <asm/processor.h>

#include "cpuid_scanner.h"

/*
 * Default CPUID scanner read function
 */
static void cpuid_read_generic(const struct cpuid_scan_entry *e, struct cpuid_read_output *output)
{
	output->info->nr_entries = 0;
	for (int i = 0; i < e->maxcnt; i++, output->leaf++, output->info->nr_entries++)
		cpuid_subleaf(e->leaf, e->subleaf + i, output->leaf);
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
