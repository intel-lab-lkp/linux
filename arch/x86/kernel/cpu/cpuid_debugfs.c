// SPDX-License-Identifier: GPL-2.0
/*
 * CPUID scanner debugfs entries: x86/scanned_cpuid/[0-ncpus]
 *
 * Dump each CPU's scanned CPUID table and compare cached values against
 * current CPUID output.  Mark changed entries with an asterisk.
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/cpuid.h>
#include <asm/cpuid/internal_api.h>
#include <asm/percpu.h>
#include <asm/processor.h>

#include "cpuid_scanner.h"

static void cpuid_this_cpu(void *info)
{
	struct cpuid_regs *regs = info;

	__cpuid(&regs->eax, &regs->ebx, &regs->ecx, &regs->edx);
};

static void
cpuid_show_leaf(struct seq_file *m, uintptr_t cpu_id, const struct leaf_query_info *info,
		const struct cpuid_regs *cached, const struct cpuid_scan_entry *entry)
{
	for (int j = 0; j < info->nr_entries; j++) {
		u32 subleaf = entry->subleaf + j;
		struct cpuid_regs regs = {
			.eax = entry->leaf,
			.ecx = subleaf,
		};

		smp_call_function_single(cpu_id, cpuid_this_cpu, &regs, true);

		seq_printf(m, "Leaf 0x%08x, subleaf %u:\n", entry->leaf, subleaf);

		seq_printf(m, "cached: EAX=0x%08x%s\tEBX=0x%08x%s\tECX=0x%08x%s\tEDX=0x%08x%s\n",
			   cached[j].eax, cached[j].eax == regs.eax ? "" : "*",
			   cached[j].ebx, cached[j].ebx == regs.ebx ? "" : "*",
			   cached[j].ecx, cached[j].ecx == regs.ecx ? "" : "*",
			   cached[j].edx, cached[j].edx == regs.edx ? "" : "*");
		seq_printf(m, "actual: EAX=0x%08x\tEBX=0x%08x\tECX=0x%08x\tEDX=0x%08x\n",
			   regs.eax, regs.ebx, regs.ecx, regs.edx);
	}
}

static int cpuid_debug_show(struct seq_file *m, void *p)
{
	uintptr_t cpu_id = (uintptr_t)m->private;
	const struct cpuinfo_x86 *c = per_cpu_ptr(&cpu_info, cpu_id);
	const struct cpuid_leaves *leaves = &c->cpuid_table.leaves;
	const struct cpuid_scan_entry *entry = cpuid_common_scan_entries;

	for (unsigned int i = 0; i < cpuid_common_scan_entries_size; i++, entry++) {
		const struct leaf_query_info *info = cpuid_leaves_info_p(leaves, entry->info_offs);
		const struct cpuid_regs *leaf = cpuid_leaves_leaf_p(leaves, entry->leaf_offs);

		cpuid_show_leaf(m, cpu_id, info, leaf, entry);
	}

	return 0;
}

static int cpuid_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpuid_debug_show, inode->i_private);
}

static const struct file_operations cpuid_ops = {
	.open		= cpuid_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static __init int cpuid_init_debugfs(void)
{
	struct dentry *base, *dir;
	uintptr_t cpu_id;
	char cpu_name[24];

	base = debugfs_create_dir("scanned_cpuid", arch_debugfs_dir);
	dir = debugfs_create_dir("cpus", base);

	for_each_possible_cpu(cpu_id) {
		scnprintf(cpu_name, sizeof(cpu_name), "%lu", cpu_id);
		debugfs_create_file(cpu_name, 0444, dir, (void *)cpu_id, &cpuid_ops);
	}

	return 0;
}
late_initcall(cpuid_init_debugfs);
