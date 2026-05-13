// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Linaro Ltd.
 *
 * Exynos850 PMU support
 */

#include <linux/soc/samsung/exynos-pmu.h>
#include <linux/soc/samsung/exynos-regs-pmu.h>
#include <linux/regmap.h>
#include <asm/cputype.h>

#include "exynos-pmu.h"

static int exynos850_cpu_pmu_offline(struct exynos_pmu_context *pmu_context, unsigned int cpu)
	__must_hold(&pmu_context->cpupm_lock)
{
	u32 this_cluster = MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 2);
	u32 cluster_cpu = MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 1);
	unsigned int cpuhint = smp_processor_id();
	u32 reg, mask;

	/* set cpu inform hint */
	regmap_write(pmu_context->pmureg, EXYNOS850_CPU_INFORM(cpuhint),
		     CPU_INFORM_C2);

	mask = BIT(cpu);
	regmap_update_bits(pmu_context->pmuintrgen, EXYNOS_GRP2_INTR_BID_ENABLE,
			   mask, BIT(cpu));

	regmap_read(pmu_context->pmuintrgen, EXYNOS_GRP1_INTR_BID_UPEND, &reg);
	regmap_write(pmu_context->pmuintrgen, EXYNOS_GRP1_INTR_BID_CLEAR,
		     reg & mask);

	mask = (BIT(cpu + 8));
	regmap_read(pmu_context->pmuintrgen, EXYNOS_GRP1_INTR_BID_UPEND, &reg);
	regmap_write(pmu_context->pmuintrgen, EXYNOS_GRP1_INTR_BID_CLEAR,
		     reg & mask);

	regmap_update_bits(pmu_context->pmureg,
			   EXYNOS850_CLUSTER_CPU_INT_EN(this_cluster, cluster_cpu),
			   1 << 3, 1 << 3);
	return 0;
}

static int exynos850_cpu_pmu_online(struct exynos_pmu_context *pmu_context, unsigned int cpu)
	__must_hold(&pmu_context->cpupm_lock)
{
	u32 this_cluster = MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 2);
	u32 cluster_cpu = MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 1);
	unsigned int cpuhint = smp_processor_id();
	u32 reg, mask;

	/* clear cpu inform hint */
	regmap_write(pmu_context->pmureg, EXYNOS850_CPU_INFORM(cpuhint),
		     CPU_INFORM_CLEAR);

	mask = BIT(cpu);

	regmap_update_bits(pmu_context->pmuintrgen, EXYNOS_GRP2_INTR_BID_ENABLE,
			   mask, (0 << cpu));

	regmap_read(pmu_context->pmuintrgen, EXYNOS_GRP2_INTR_BID_UPEND, &reg);

	regmap_write(pmu_context->pmuintrgen, EXYNOS_GRP2_INTR_BID_CLEAR,
		     reg & mask);

	regmap_update_bits(pmu_context->pmureg,
			   EXYNOS850_CLUSTER_CPU_INT_EN(this_cluster, cluster_cpu),
			   1 << 3, 0 << 3);
	return 0;
}

const struct exynos_pmu_data exynos850_pmu_data = {
	.pmu_cpuhp = true,
	.cpu_pmu_offline = exynos850_cpu_pmu_offline,
	.cpu_pmu_online = exynos850_cpu_pmu_online,
};
