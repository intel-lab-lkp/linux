// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/mm.h>

#include <asm/cpufeature.h>
#include <asm/cpuid/api.h>
#include <asm/msr.h>

#include "cpu.h"

/*
 * If CPU revision is 0x02000000, then CPUID(0x80860002) should be used instead.
 */
static bool is_legacy_revision(const struct leaf_0x80860001_0 *l1)
{
	return !(l1->cpu_rev_major == 2 && l1->cpu_rev_minor == 0 &&
		 l1->cpu_rev_mask_major == 0 && l1->cpu_rev_mask_minor == 0);
}

static void print_cpu_revision(struct cpuinfo_x86 *c)
{
	const struct leaf_0x80860001_0 *l1 = cpuid_leaf(c, 0x80860001);
	const struct leaf_0x80860002_0 *l2 = cpuid_leaf(c, 0x80860002);

	if (l1 && is_legacy_revision(l1)) {
		pr_info("CPU: Processor revision %u.%u.%u.%u, %u MHz\n",
			l1->cpu_rev_major, l1->cpu_rev_minor,
			l1->cpu_rev_mask_major, l1->cpu_rev_mask_minor,
			l1->cpu_base_mhz);
	}

	if (l1 && l2 && !is_legacy_revision(l1)) {
		pr_info("CPU: Processor revision %08X, %u MHz\n",
			l2->cpu_rev_id, l1->cpu_base_mhz);
	}

	if (l2) {
		pr_info("CPU: Code Morphing Software revision %u.%u.%u-%u-%u\n",
			l2->cms_rev_major, l2->cms_rev_minor,
			l2->cms_rev_mask_1, l2->cms_rev_mask_2,
			l2->cms_rev_mask_3);
	}
}

static void print_cpu_info_string(struct cpuinfo_x86 *c)
{
	const struct leaf_0x80860003_0 *l3 = cpuid_leaf(c, 0x80860003);
	const struct leaf_0x80860004_0 *l4 = cpuid_leaf(c, 0x80860004);
	const struct leaf_0x80860005_0 *l5 = cpuid_leaf(c, 0x80860005);
	const struct leaf_0x80860006_0 *l6 = cpuid_leaf(c, 0x80860006);

	if (l3 && l4 && l5 && l6) {
		u32 info[] = {
			l3->cpu_info_0,  l3->cpu_info_1,  l3->cpu_info_2,  l3->cpu_info_3,
			l4->cpu_info_4,  l4->cpu_info_5,  l4->cpu_info_6,  l4->cpu_info_7,
			l5->cpu_info_8,  l5->cpu_info_9,  l5->cpu_info_10, l5->cpu_info_11,
			l6->cpu_info_12, l6->cpu_info_13, l6->cpu_info_14, l6->cpu_info_15,
			0 /* Null terminator */,
		};
		pr_info("CPU: %s\n", (char *)info);
	}
}

static void init_transmeta(struct cpuinfo_x86 *c)
{
	unsigned int cap_mask, uk;

	early_init_transmeta(c);
	cpu_detect_cache_sizes(c);

	print_cpu_revision(c);
	print_cpu_info_string(c);

	/* Unhide possibly hidden capability flags */
	rdmsr(0x80860004, cap_mask, uk);
	wrmsr(0x80860004, ~0, uk);
	cpuid_refresh_leaf(c, 0x1);
	wrmsr(0x80860004, cap_mask, uk);

	/* All Transmeta CPUs have a constant TSC */
	set_cpu_cap(c, X86_FEATURE_CONSTANT_TSC);

#ifdef CONFIG_SYSCTL
	/*
	 * randomize_va_space slows us down enormously;
	 * it probably triggers retranslation of x86->native bytecode
	 */
	randomize_va_space = 0;
#endif
}

static const struct cpu_dev transmeta_cpu_dev = {
	.c_vendor	= "Transmeta",
	.c_ident	= { "GenuineTMx86", "TransmetaCPU" },
	.c_init		= init_transmeta,
	.c_x86_vendor	= X86_VENDOR_TRANSMETA,
};

cpu_dev_register(transmeta_cpu_dev);
