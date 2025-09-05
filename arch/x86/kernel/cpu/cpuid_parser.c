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
 * Leaf read functions:
 */

/*
 * Default CPUID parser read function
 *
 * Satisfies the requirements stated at 'struct cpuid_parse_entry'->read().
 */
static void cpuid_read_generic(const struct cpuid_parse_entry *e, struct cpuid_read_output *output)
{
	for (int i = 0; i < e->maxcnt; i++, output->regs++, output->info->nr_entries++)
		cpuid_read_subleaf(e->leaf, e->subleaf + i, output->regs);
}

/*
 * CPUID parser tables:
 *
 * Since these tables reference the leaf read functions above, they must be
 * defined afterwards.
 */

static const struct cpuid_parse_entry cpuid_parse_entries[] = {
	CPUID_PARSE_ENTRIES
};

/*
 * Leaf-independent parser code:
 */

static unsigned int cpuid_range_max_leaf(const struct cpuid_table *t, unsigned int range)
{
	const struct leaf_0x0_0 *l0 = __cpuid_table_subleaf(t, 0x0, 0);

	switch (range) {
	case CPUID_BASE_START:	return l0  ?  l0->max_std_leaf : 0;
	default:		return 0;
	}
}

static bool
cpuid_range_valid(const struct cpuid_table *t, unsigned int leaf, unsigned int start, unsigned int end)
{
	if (leaf < start || leaf > end)
		return false;

	return leaf == start || leaf <= cpuid_range_max_leaf(t, start);
}

static bool cpuid_leaf_in_range(const struct cpuid_table *t, unsigned int leaf)
{
	return cpuid_range_valid(t, leaf, CPUID_BASE_START, CPUID_BASE_END);
}

static void
cpuid_fill_table(struct cpuid_table *t, const struct cpuid_parse_entry entries[], unsigned int nr_entries)
{
	const struct cpuid_parse_entry *entry = entries;

	for (unsigned int i = 0; i < nr_entries; i++, entry++) {
		struct cpuid_read_output output = {
			.regs	= cpuid_table_query_regs_p(t, entry->regs_offs),
			.info	= cpuid_table_query_info_p(t, entry->info_offs),
		};

		if (!cpuid_leaf_in_range(t, entry->leaf))
			continue;

		WARN_ON_ONCE(output.info->nr_entries != 0);
		entry->read(entry, &output);
	}
}

/*
 * Exported APIs:
 */

/**
 * cpuid_parser_scan_cpu() - Populate current CPU's CPUID table
 * @c:		CPU capability structure associated with the current CPU
 *
 * Populate the CPUID table embedded within @c with parsed CPUID data.  Since all CPUID
 * instructions are invoked locally, this must be called on the CPU associated with @c.
 */
void cpuid_parser_scan_cpu(struct cpuinfo_x86 *c)
{
	struct cpuid_table *table = &c->cpuid;

	/*
	 * For correctness, clear the CPUID table first.
	 *
	 * This is due to the CPUID parser APIs at <asm/cpuid/api.h> using leaf->nr_entries
	 * as a leaf validity check: non-zero means that the CPUID leaf's cached output is
	 * valid.  Otherwise, NULL is returned.
	 *
	 * For the primary CPU's early boot code, the tables are already zeroed.  For
	 * secondary CPUs though, their capability structures (containing the CPUID table)
	 * are copied from the primary CPU.  This would result in a leaf->nr_entries value
	 * carry over, unless the table is zeroed first.
	 *
	 * Also for CPUID table re-scans, which are triggered by hardware state changes,
	 * previously valid CPUID leaves can become no longer available and thus no longer
	 * parsed (leaving stale leaf "nr_entries" fields behind.)  The table must thus be
	 * also cleared.
	 */
	memset(table, 0, sizeof(*table));

	cpuid_fill_table(table, cpuid_parse_entries, ARRAY_SIZE(cpuid_parse_entries));
}
