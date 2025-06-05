// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Centralized CPUID parser (for populating the system's CPUID tables.)
 */

#include <linux/init.h>
#include <linux/kernel.h>

#include <asm/cpuid/api.h>
#include <asm/percpu.h>
#include <asm/processor.h>

#include "cpuid_parser.h"

/*
 * Default CPUID parser read function
 */
static void cpuid_read_generic(const struct cpuid_parse_entry *e, struct cpuid_read_output *output)
{
	output->info->nr_entries = 0;
	for (int i = 0; i < e->maxcnt; i++, output->regs++, output->info->nr_entries++)
		cpuid_read_subleaf(e->leaf, e->subleaf + i, output->regs);
}

static unsigned int cpuid_range_max_leaf(const struct cpuid_leaves *l, unsigned int range)
{
	switch (range) {
	case CPUID_BASE_START:	return __cpuid_leaves_subleaf_0(l, 0x0).max_std_leaf;
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
	return cpuid_range_valid(l, leaf, CPUID_BASE_START, CPUID_BASE_END);
}

static const struct cpuid_parse_entry cpuid_common_parse_entries[] = {
	CPUID_PARSE_ENTRIES
};

static void
cpuid_fill_table(struct cpuid_table *t, const struct cpuid_parse_entry entries[], unsigned int nr_entries)
{
	const struct cpuid_parse_entry *entry = entries;

	for (unsigned int i = 0; i < nr_entries; i++, entry++) {
		struct cpuid_read_output output = {
			.regs	= cpuid_leaves_query_regs_p(&t->leaves, entry->regs_offs),
			.info	= cpuid_leaves_query_info_p(&t->leaves, entry->info_offs),
		};

		if (!cpuid_leaf_valid(&t->leaves, entry->leaf))
			continue;

		entry->read(entry, &output);
	}
}

/**
 * cpuid_parser_scan_cpu() - Populate current CPU's CPUID table
 * @c:		CPU capability structure associated with the current CPU
 *
 * Populate the CPUID table embedded within @c with parsed CPUID data.  Since all CPUID
 * instructions are invoked locally, this must be called on the CPU associated with @c.
 */
void cpuid_parser_scan_cpu(struct cpuinfo_x86 *c)
{
	cpuid_fill_table(&c->cpuid, cpuid_common_parse_entries,
			 ARRAY_SIZE(cpuid_common_parse_entries));
}
