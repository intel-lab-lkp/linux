// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Centralized CPUID parser (for populating the system's CPUID tables.)
 */

#define pr_fmt(fmt) "x86/cpuid: " fmt

#include <linux/init.h>
#include <linux/kernel.h>

#include <asm/cpuid/api.h>
#include <asm/percpu.h>
#include <asm/processor.h>

#include "cpuid_parser.h"

static const struct cpuid_vendor_entry cpuid_vendor_entries[] = {
	CPUID_VENDOR_ENTRIES
};

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

static void cpuid_read_0x2(const struct cpuid_parse_entry *e, struct cpuid_read_output *output)
{
	union leaf_0x2_regs *regs = (union leaf_0x2_regs *)output->regs;
	struct leaf_0x2_0 *l = (struct leaf_0x2_0 *)output->regs;
	int invalid_regs = 0;

	/*
	 * All Intel CPUs must report an iteration count of 1.  For broken hardware,
	 * keep the leaf marked as invalid at the CPUID table.
	 */
	cpuid_read_subleaf(e->leaf, e->subleaf, l);
	if (l->iteration_count != 0x01) {
		pr_warn_once("Ignoring CPUID(0x2) due to invalid iteration count = %d",
			     l->iteration_count);
		return;
	}

	/*
	 * The most significant bit (MSB) of each CPUID(0x2) register must be clear.
	 * If a register is malformed, replace its 1-byte descriptors with NULL.
	 */
	for (int i = 0; i < 4; i++) {
		if (regs->reg[i].invalid) {
			regs->regv[i] = 0;
			invalid_regs++;
		}
	}

	/*
	 * If all of the CPUID(0x2) output registers were malformed, keep the leaf
	 * marked as invalid at the CPUID table.
	 */
	if (invalid_regs == 4)
		return;

	output->info->nr_entries = 1;
}

static void cpuid_read_0x80000000(const struct cpuid_parse_entry *e, struct cpuid_read_output *output)
{
	struct leaf_0x80000000_0 *el0 = (struct leaf_0x80000000_0 *)output->regs;

	cpuid_read_subleaf(e->leaf, e->subleaf, el0);

	/*
	 * Protect against Intel 32-bit CPUs lacking an extended CPUID range. A
	 * CPUID(0x80000000) query on such machines will just repeat the output
	 * of the highest standard CPUID leaf.
	 */
	if ((el0->max_ext_leaf & 0xffff0000) != 0x80000000)
		return;

	output->info->nr_entries = 1;
}

/*
 * CPUID parser tables:
 *
 * Since these tables reference the leaf read functions above, they must be
 * defined afterwards.
 *
 * At early boot, only leaves at CPUID_EARLY_PARSE_ENTRIES should be parsed.
 */

static const struct cpuid_parse_entry cpuid_early_parse_entries[] = {
	CPUID_EARLY_PARSE_ENTRIES
};

static const struct cpuid_parse_entry cpuid_common_parse_entries[] = {
	CPUID_COMMON_PARSE_ENTRIES
};

const struct cpuid_parser_phase cpuid_parser_phases[] = {
	{ cpuid_early_parse_entries,	ARRAY_SIZE(cpuid_early_parse_entries)	},
	{ cpuid_common_parse_entries,	ARRAY_SIZE(cpuid_common_parse_entries)	},
};

const int cpuid_parser_nphases = ARRAY_SIZE(cpuid_parser_phases);

/*
 * Leaf-independent parser code:
 */

static unsigned int cpuid_range_max_leaf(const struct cpuid_table *t, unsigned int range)
{
	const struct leaf_0x0_0 *l0 = __cpuid_table_subleaf(t, 0x0, 0);
	const struct leaf_0x80000000_0 *el0 = __cpuid_table_subleaf(t, 0x80000000, 0);

	switch (range) {
	case CPUID_BASE_START:	return l0  ?  l0->max_std_leaf : 0;
	case CPUID_EXT_START:	return el0 ? el0->max_ext_leaf : 0;
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
	return cpuid_range_valid(t, leaf, CPUID_BASE_START, CPUID_BASE_END) ||
	       cpuid_range_valid(t, leaf, CPUID_EXT_START, CPUID_EXT_END);
}

static bool cpuid_leaf_matches_vendor(unsigned int leaf, u8 cpu_vendor)
{
	const struct cpuid_parse_entry *p = cpuid_early_parse_entries;
	const struct cpuid_vendor_entry *v = cpuid_vendor_entries;

	/* Leaves in the early boot parser table are vendor agnostic */
	for (int i = 0; i < ARRAY_SIZE(cpuid_early_parse_entries); i++, p++)
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

		if (!cpuid_leaf_matches_vendor(entry->leaf, boot_cpu_data.x86_vendor))
			continue;

		WARN_ON_ONCE(output.info->nr_entries != 0);
		entry->read(entry, &output);
	}
}

/*
 * Exported APIs:
 */

static void __cpuid_parser_scan_cpu(struct cpuinfo_x86 *c, bool early_boot)
{
	int nphases = early_boot ? 1 : cpuid_parser_nphases;
	struct cpuid_table *table = &c->cpuid;

	/*
	 * After early boot, clear the CPUID table first.
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
	if (!early_boot)
		memset(table, 0, sizeof(*table));

	for (int i = 0; i < nphases; i++)
		cpuid_fill_table(table, cpuid_parser_phases[i].table, cpuid_parser_phases[i].nr_entries);
}

/**
 * cpuid_parser_scan_cpu() - Populate the current CPU's CPUID table
 * @c:		CPU capability structure for the current CPU
 *
 * Populate the CPUID table embedded within @c with parsed CPUID data.	Since all CPUID
 * instructions are invoked locally, this must be run on the CPU associated with @c.
 *
 * cpuid_parser_early_scan_cpu() must've been called, at least once, beforehand.
 */
void cpuid_parser_scan_cpu(struct cpuinfo_x86 *c)
{
	__cpuid_parser_scan_cpu(c, false);
}

/**
 * cpuid_parser_early_scan_cpu() - Populate primary CPU's CPUID table on early boot
 * @c:		CPU capability structure associated with the current CPU
 *
 * Populate the CPUID table embedded within @c with parsed CPUID data.
 *
 * This must be called at early boot, so that the boot code can identify the CPU's
 * x86 vendor.	Only CPUID(0x0) and CPUID(0x1) are parsed.
 *
 * After saving the x86 vendor info in the boot CPU's capability structure,
 * cpuid_parser_scan_cpu() must be called to complete the CPU's CPUID table.
 */
void __init cpuid_parser_early_scan_cpu(struct cpuinfo_x86 *c)
{
	__cpuid_parser_scan_cpu(c, true);
}
