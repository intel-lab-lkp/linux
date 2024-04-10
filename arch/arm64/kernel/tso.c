// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2024 Apple Inc. All rights reserved.
 */

#include <linux/types.h>

#include <asm/cputype.h>
#include <asm/processor.h>
#include <asm/sysreg.h>
#include <asm/tso.h>

#ifdef CONFIG_ARM64_TSO

static bool tso_supported(void)
{
	unsigned int cpuid_implementor = read_cpuid_implementor();
	u64 aidr = read_sysreg(aidr_el1);

	return (cpuid_implementor == ARM_CPU_IMP_APPLE) &&
		(aidr & SYS_AIDR_EL1_TSO_MASK);
}

static int tso_enabled(void)
{
	if (!tso_supported())
		return -EOPNOTSUPP;

	u64 actlr_el1 = read_sysreg(actlr_el1);

	return !!(actlr_el1 & SYS_ACTLR_EL1_TSOEN_MASK);
}

int modify_tso_enable(bool tso_enable)
{
	if (!tso_supported())
		return -EOPNOTSUPP;

	u64 actlr_el1_old = read_sysreg(actlr_el1);
	u64 actlr_el1_new =
		(actlr_el1_old & ~SYS_ACTLR_EL1_TSOEN_MASK) |
		(tso_enable << SYS_ACTLR_EL1_TSOEN_SHIFT);

	write_sysreg(actlr_el1_new, actlr_el1);

	if (tso_enabled() != tso_enable)
		return -EOPNOTSUPP;

	return 0;
}

#endif /* CONFIG_ARM64_TSO */
