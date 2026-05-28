// SPDX-License-Identifier: GPL-2.0
#include <linux/sched.h>
#include <linux/sched/clock.h>

#include <asm/cpu.h>
#include <asm/cpufeature.h>
#include <asm/cpuid/api.h>
#include <asm/msr.h>

#include "cpu.h"

#define MSR_ZHAOXIN_FCR57 0x00001257

#define ACE_FCR		(1 << 7)	/* MSR_ZHAOXIN_FCR */
#define RNG_ENABLE	(1 << 8)	/* MSR_ZHAOXIN_RNG */

static void init_zhaoxin_cap(struct cpuinfo_x86 *c)
{
	const struct leaf_0xc0000001_0 *l1 = cpuid_leaf(c, 0xc0000001);
	u32 lo, hi;

	if (l1) {
		/* Enable ACE unit, if present and disabled */
		if (l1->ace && !l1->ace_en) {
			rdmsr(MSR_ZHAOXIN_FCR57, lo, hi);
			lo |= ACE_FCR;
			wrmsr(MSR_ZHAOXIN_FCR57, lo, hi);
			pr_info("CPU: Enabled ACE h/w crypto\n");
		}

		/* Enable RNG unit, if present and disabled */
		if (l1->rng && !l1->rng_en) {
			rdmsr(MSR_ZHAOXIN_FCR57, lo, hi);
			lo |= RNG_ENABLE;
			wrmsr(MSR_ZHAOXIN_FCR57, lo, hi);
			pr_info("CPU: Enabled h/w RNG\n");
		}

		/*
		 * Force-enabling CPU features affects the CPUID(0xc0000001)
		 * EDX feature bits.  Refresh the leaf.
		 */
		cpuid_refresh_leaf(c, 0xc0000001);
		c->x86_capability[CPUID_C000_0001_EDX] = cpuid_edx(0xC0000001);
	}

	if (c->x86 >= 0x6)
		set_cpu_cap(c, X86_FEATURE_REP_GOOD);
}

static void early_init_zhaoxin(struct cpuinfo_x86 *c)
{
	if (c->x86 >= 0x6)
		set_cpu_cap(c, X86_FEATURE_CONSTANT_TSC);

	if (c->x86_power & (1 << 8)) {
		set_cpu_cap(c, X86_FEATURE_CONSTANT_TSC);
		set_cpu_cap(c, X86_FEATURE_NONSTOP_TSC);
	}
}

static void init_zhaoxin(struct cpuinfo_x86 *c)
{
	const struct leaf_0xa_0 *la = cpuid_leaf(c, 0xa);

	early_init_zhaoxin(c);
	init_intel_cacheinfo(c);

	if (la && la->pmu_version && la->num_counters_gp > 1)
		set_cpu_cap(c, X86_FEATURE_ARCH_PERFMON);

	if (c->x86 >= 0x6)
		init_zhaoxin_cap(c);
#ifdef CONFIG_X86_64
	set_cpu_cap(c, X86_FEATURE_LFENCE_RDTSC);
#endif

	init_ia32_feat_ctl(c);
}

#ifdef CONFIG_X86_32
static unsigned int
zhaoxin_size_cache(struct cpuinfo_x86 *c, unsigned int size)
{
	return size;
}
#endif

static const struct cpu_dev zhaoxin_cpu_dev = {
	.c_vendor	= "zhaoxin",
	.c_ident	= { "  Shanghai  " },
	.c_early_init	= early_init_zhaoxin,
	.c_init		= init_zhaoxin,
#ifdef CONFIG_X86_32
	.legacy_cache_size = zhaoxin_size_cache,
#endif
	.c_x86_vendor	= X86_VENDOR_ZHAOXIN,
};

cpu_dev_register(zhaoxin_cpu_dev);
