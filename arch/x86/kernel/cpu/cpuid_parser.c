// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * CPUID parser; for populating the system's CPUID tables.
 */

#include <linux/kernel.h>

#include <asm/cpuid/api.h>
#include <asm/processor.h>

#include "cpuid_parser.h"

/* Clear a single CPUID table entry */
static void cpuid_clear(const struct cpuid_parse_entry *e, const struct cpuid_read_output *output)
{
	struct cpuid_regs *regs = output->regs;

	for (int i = 0; i < e->maxcnt; i++, regs++)
		memset(regs, 0, sizeof(*regs));

	memset(output->info, 0, sizeof(*output->info));
}

static const struct cpuid_vendor_entry cpuid_vendor_entries[] = {
	CPUID_VENDOR_ENTRIES
};

/*
 * Leaf read functions:
 */

/*
 * Default CPUID read function
 * Satisfies the requirements stated at 'struct cpuid_parse_entry'->read().
 */
static void
cpuid_read_generic(const struct cpuid_parse_entry *e, const struct cpuid_read_output *output)
{
	struct cpuid_regs *regs = output->regs;

	for (int i = 0; i < e->maxcnt; i++, regs++, output->info->nr_entries++)
		cpuid_read_subleaf(e->leaf, e->subleaf + i, regs);
}

/*
 * Define an extended range CPUID read function
 *
 * Guard against CPUs lacking the passed range leaf; e.g. Intel 32-bit CPUs lacking
 * CPUID(0x80000000).  A query on such machines will just repeat the output of the
 * highest standard CPUID leaf.
 */
#define define_cpuid_range_read_function(_range, _name)					\
static void											\
cpuid_read_##_range(const struct cpuid_parse_entry *e, const struct cpuid_read_output *output)	\
{												\
	struct leaf_##_range##_0 *l = (struct leaf_##_range##_0 *)output->regs;			\
												\
	cpuid_read_subleaf(e->leaf, e->subleaf, l);						\
	if (CPUID_RANGE(l->max_##_name##_leaf) != _range)					\
		return;										\
												\
	output->info->nr_entries = 1;								\
}

define_cpuid_range_read_function(0x80000000, ext);
define_cpuid_range_read_function(0x80860000, tra);
define_cpuid_range_read_function(0xc0000000, cntr);

/*
 * CPUID parser tables:
 *
 * At early boot, only leaves at cpuid_early_entries[] should be parsed.
 */

static const struct cpuid_parse_entry cpuid_early_entries[] = {
	CPUID_EARLY_ENTRIES
};

static const struct cpuid_parse_entry cpuid_common_entries[] = {
	CPUID_COMMON_ENTRIES
};

const struct cpuid_phase cpuid_phases[] = {
	{	cpuid_early_entries,	ARRAY_SIZE(cpuid_early_entries)		},
	{	cpuid_common_entries,	ARRAY_SIZE(cpuid_common_entries)	},
};

const int cpuid_nphases = ARRAY_SIZE(cpuid_phases);

/*
 * Leaf-independent parser code:
 */

static bool cpuid_leaf_matches_vendor(unsigned int leaf, u8 cpu_vendor)
{
	const struct cpuid_parse_entry *p = cpuid_early_entries;
	const struct cpuid_vendor_entry *v = cpuid_vendor_entries;

	/* Leaves in the early boot parser table are vendor agnostic */
	for (int i = 0; i < ARRAY_SIZE(cpuid_early_entries); i++, p++)
		if (p->leaf == leaf)
			return true;

	/* Leaves in the vendor table must pass a CPU vendor check */
	for (int i = 0; i < ARRAY_SIZE(cpuid_vendor_entries); i++, v++) {
		if (v->leaf != leaf)
			continue;

		for (unsigned int j = 0; j < v->nvendors; j++)
			if (cpu_vendor == v->vendors[j])
				return true;

		return false;
	}

	/* Remaining leaves are vendor agnostic */
	return true;
}

static unsigned int cpuid_range_max_leaf(const struct cpuid_table *t, unsigned int range)
{
	const struct leaf_0x0_0 *l0 = __cpuid_table_subleaf(t, 0x0, 0);
	const struct leaf_0x80000000_0 *el0 = __cpuid_table_subleaf(t, 0x80000000, 0);
	const struct leaf_0x80860000_0 *tl0 = __cpuid_table_subleaf(t, 0x80860000, 0);
	const struct leaf_0xc0000000_0 *cl0 = __cpuid_table_subleaf(t, 0xc0000000, 0);

	switch (range) {
	case CPUID_BASE_START:	return l0  ?  l0->max_std_leaf  : 0;
	case CPUID_EXT_START:	return el0 ? el0->max_ext_leaf  : 0;
	case CPUID_TMX_START:	return tl0 ? tl0->max_tra_leaf  : 0;
	case CPUID_CTR_START:	return cl0 ? cl0->max_cntr_leaf : 0;
	default:		return 0;
	}
}

static void
__cpuid_reset_table(struct cpuid_table *t, const struct cpuid_parse_entry entries[],
		    unsigned int nr_entries, unsigned int start, unsigned int end, bool fill)
{
	const struct cpuid_parse_entry *entry = entries;
	unsigned int range = CPUID_RANGE(start);

	for (unsigned int i = 0; i < nr_entries; i++, entry++) {
		struct cpuid_read_output output = {
			.regs = cpuid_table_regs_p(t, entry->regs_offs),
			.info = cpuid_table_info_p(t, entry->info_offs),
		};

		if (entry->leaf < start || entry->leaf > end)
			continue;

		if (!cpuid_leaf_matches_vendor(entry->leaf, boot_cpu_data.x86_vendor))
			continue;

		cpuid_clear(entry, &output);

		/*
		 * Read the range's anchor leaf unconditionally so that the cached
		 * maximum valid leaf value is available for the remaining entries.
		 */
		if (fill && (entry->leaf == range || entry->leaf <= cpuid_range_max_leaf(t, range)))
			entry->read(entry, &output);
	}
}

/*
 * Zero all cached CPUID entries within [@start-@end] range.  This is needed when
 * certain operations like MSR writes induce changes to the CPU's CPUID layout.
 */
static void
__cpuid_zero_table(struct cpuid_table *t, const struct cpuid_parse_entry entries[],
		   unsigned int nr_entries, unsigned int start, unsigned int end)
{
	__cpuid_reset_table(t, entries, nr_entries, start, end, false);
}

static void
__cpuid_fill_table(struct cpuid_table *t, const struct cpuid_parse_entry entries[],
		   unsigned int nr_entries, unsigned int start, unsigned int end)
{
	__cpuid_reset_table(t, entries, nr_entries, start, end, true);
}

static void
cpuid_fill_table(struct cpuid_table *t, const struct cpuid_parse_entry entries[], unsigned int nr_entries)
{
	static const struct {
		unsigned int start;
		unsigned int end;
	} ranges[] = {
		{ CPUID_BASE_START, CPUID_BASE_END },
		{ CPUID_EXT_START,  CPUID_EXT_END  },
		{ CPUID_TMX_START,  CPUID_TMX_END  },
		{ CPUID_CTR_START,  CPUID_CTR_END  },
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(ranges); i++)
		__cpuid_fill_table(t, entries, nr_entries, ranges[i].start, ranges[i].end);
}

static void __cpuid_scan_cpu_full(struct cpuinfo_x86 *c, bool early_boot)
{
	int nphases = early_boot ? 1 : cpuid_nphases;
	struct cpuid_table *table = &c->cpuid;

	for (int i = 0; i < nphases; i++)
		cpuid_fill_table(table, cpuid_phases[i].table, cpuid_phases[i].nr_entries);
}

static void
__cpuid_scan_cpu_partial(struct cpuinfo_x86 *c, bool early_boot, unsigned int start_leaf, unsigned int end_leaf)
{
	int nphases = early_boot ? 1 : ARRAY_SIZE(cpuid_phases);
	struct cpuid_table *table = &c->cpuid;

	for (int i = 0; i < nphases; i++) {
		const struct cpuid_parse_entry *entries = cpuid_phases[i].table;
		unsigned int nr_entries = cpuid_phases[i].nr_entries;

		__cpuid_zero_table(table, entries, nr_entries, start_leaf, end_leaf);
		__cpuid_fill_table(table, entries, nr_entries, start_leaf, end_leaf);
	}
}

/*
 * Call-site APIs:
 */

/**
 * cpuid_scan_cpu_early() - Populate CPUID table on early boot
 * @c:		CPU capability structure associated with the current CPU
 *
 * Populate the CPUID table embedded within @c with parsed CPUID data.
 *
 * This must be called at early boot, so that early boot code can identify the
 * CPU's x86 vendor.  Only CPUID(0x0) and CPUID(0x1) are parsed.
 *
 * cpuid_scan_cpu() must be called later to complete the CPUID table.  That is,
 * after saving the x86 vendor info to the CPU capability structure @c.
 */
void cpuid_scan_cpu_early(struct cpuinfo_x86 *c)
{
	__cpuid_scan_cpu_full(c, true);
}

/**
 * cpuid_scan_cpu() - Populate current CPU's CPUID table
 * @c:		CPU capability structure associated with the current CPU
 *
 * Populate the CPUID table embedded within @c with parsed CPUID data.  All CPUID
 * instructions are invoked locally, so this must be called on the CPU associated
 * with @c.
 *
 * cpuid_scan_cpu_early() must have been called earlier on @c.
 */
void cpuid_scan_cpu(struct cpuinfo_x86 *c)
{
	__cpuid_scan_cpu_full(c, false);
}

/**
 * cpuid_refresh_range() - Rescan a CPUID table's leaf range
 * @c:		CPU capability structure associated with the current CPU
 * @start:	Start of leaf range to be re-scanned
 * @end:	End of leaf range
 *
 * cpuid_scan_cpu_early() must have been called earlier on @c.
 */
void cpuid_refresh_range(struct cpuinfo_x86 *c, u32 start, u32 end)
{
	if (WARN_ON_ONCE(start > end))
		return;

	if (WARN_ON_ONCE(CPUID_RANGE(start) != CPUID_RANGE(end)))
		return;

	__cpuid_scan_cpu_partial(c, false, start, end);
}

/**
 * cpuid_refresh_leaf() - Rescan a CPUID table's leaf
 * @c:		CPU capability structure associated with the current CPU
 * @leaf:	Leaf to be re-scanned
 *
 * cpuid_scan_cpu_early() must have been called earlier on @c.
 */
void cpuid_refresh_leaf(struct cpuinfo_x86 *c, u32 leaf)
{
	cpuid_refresh_range(c, leaf, leaf);
}
