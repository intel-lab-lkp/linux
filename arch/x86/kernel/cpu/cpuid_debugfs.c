// SPDX-License-Identifier: GPL-2.0
/*
 * CPUID parser debugfs entries: x86/cpuid/[0-ncpus]
 *
 * Dump each CPU's cached CPUID table and compare its values against current
 * CPUID output on that CPU.  Mark changed entries with an asterisk.
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/cpuid/api.h>
#include <asm/percpu.h>
#include <asm/processor.h>

#include "cpuid_parser.h"

static void cpuid_this_cpu(void *info)
{
	struct cpuid_regs *regs = info;

	__cpuid(&regs->eax, &regs->ebx, &regs->ecx, &regs->edx);
}

static void
cpuid_show_leaf(struct seq_file *m, uintptr_t cpu_id, const struct cpuid_parse_entry *entry,
		const struct leaf_parse_info *info, const struct cpuid_regs *cached)
{
	for (int j = 0; j < info->nr_entries; j++) {
		u32 subleaf = entry->subleaf + j;
		struct cpuid_regs regs = {
			.eax = entry->leaf,
			.ecx = subleaf,
		};
		int ret;

		seq_printf(m, "Leaf 0x%08x, subleaf %u:\n", entry->leaf, subleaf);

		ret = smp_call_function_single(cpu_id, cpuid_this_cpu, &regs, true);
		if (ret) {
			seq_printf(m, "Failed to invoke CPUID on CPU %lu: %d\n\n", cpu_id, ret);
			continue;
		}

		seq_printf(m, "  cached:   %cEAX=0x%08x   %cEBX=0x%08x   %cECX=0x%08x   %cEDX=0x%08x\n",
			   cached[j].eax == regs.eax ? ' ' : '*', cached[j].eax,
			   cached[j].ebx == regs.ebx ? ' ' : '*', cached[j].ebx,
			   cached[j].ecx == regs.ecx ? ' ' : '*', cached[j].ecx,
			   cached[j].edx == regs.edx ? ' ' : '*', cached[j].edx);
		seq_printf(m, "  actual:    EAX=0x%08x    EBX=0x%08x    ECX=0x%08x    EDX=0x%08x\n",
			   regs.eax, regs.ebx, regs.ecx, regs.edx);
	}
}

static void __cpuid_debug_show(struct seq_file *m, uintptr_t cpu_id,
			       const struct cpuid_parse_entry *entry, int nr_entries)
{
	const struct cpuinfo_x86 *c = per_cpu_ptr(&cpu_info, cpu_id);
	const struct cpuid_table *t = &c->cpuid;

	for (int i = 0; i < nr_entries; i++, entry++) {
		const struct leaf_parse_info *qi = cpuid_table_info_p(t, entry->info_offs);
		const struct cpuid_regs *qr = cpuid_table_regs_p(t, entry->regs_offs);

		cpuid_show_leaf(m, cpu_id, entry, qi, qr);
	}
}

static int cpuid_debug_show(struct seq_file *m, void *p)
{
	uintptr_t cpu_id = (uintptr_t)m->private;

	for (int i = 0; i < cpuid_nphases; i++)
		__cpuid_debug_show(m, cpu_id, cpuid_phases[i].table, cpuid_phases[i].nr_entries);

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
	struct dentry *dir;
	uintptr_t cpu_id;
	char cpu_name[24];

	dir = debugfs_create_dir("cpuid", arch_debugfs_dir);

	for_each_possible_cpu(cpu_id) {
		scnprintf(cpu_name, sizeof(cpu_name), "%lu", cpu_id);
		debugfs_create_file(cpu_name, 0444, dir, (void *)cpu_id, &cpuid_ops);
	}

	return 0;
}
late_initcall(cpuid_init_debugfs);
