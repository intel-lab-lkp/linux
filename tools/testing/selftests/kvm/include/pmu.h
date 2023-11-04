/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023, Tencent, Inc.
 */
#ifndef SELFTEST_KVM_PMU_H
#define SELFTEST_KVM_PMU_H

#include <stdint.h>

#define X86_PMC_IDX_MAX				64
#define INTEL_PMC_MAX_GENERIC				32
#define KVM_PMU_EVENT_FILTER_MAX_EVENTS		300

#define GP_COUNTER_NR_OFS_BIT				8
#define EVENT_LENGTH_OFS_BIT				24

#define PMU_VERSION_MASK				GENMASK_ULL(7, 0)
#define EVENT_LENGTH_MASK				GENMASK_ULL(31, EVENT_LENGTH_OFS_BIT)
#define GP_COUNTER_NR_MASK				GENMASK_ULL(15, GP_COUNTER_NR_OFS_BIT)
#define FIXED_COUNTER_NR_MASK				GENMASK_ULL(4, 0)

#define ARCH_PERFMON_EVENTSEL_EVENT			GENMASK_ULL(7, 0)
#define ARCH_PERFMON_EVENTSEL_UMASK			GENMASK_ULL(15, 8)
#define ARCH_PERFMON_EVENTSEL_USR			BIT_ULL(16)
#define ARCH_PERFMON_EVENTSEL_OS			BIT_ULL(17)
#define ARCH_PERFMON_EVENTSEL_EDGE			BIT_ULL(18)
#define ARCH_PERFMON_EVENTSEL_PIN_CONTROL		BIT_ULL(19)
#define ARCH_PERFMON_EVENTSEL_INT			BIT_ULL(20)
#define ARCH_PERFMON_EVENTSEL_ANY			BIT_ULL(21)
#define ARCH_PERFMON_EVENTSEL_ENABLE			BIT_ULL(22)
#define ARCH_PERFMON_EVENTSEL_INV			BIT_ULL(23)
#define ARCH_PERFMON_EVENTSEL_CMASK			GENMASK_ULL(31, 24)

#define PMC_MAX_FIXED					16
#define PMC_IDX_FIXED					32

/* RDPMC offset for Fixed PMCs */
#define PMC_FIXED_RDPMC_BASE				BIT_ULL(30)
#define PMC_FIXED_RDPMC_METRICS			BIT_ULL(29)

#define FIXED_BITS_MASK				0xFULL
#define FIXED_BITS_STRIDE				4
#define FIXED_0_KERNEL					BIT_ULL(0)
#define FIXED_0_USER					BIT_ULL(1)
#define FIXED_0_ANYTHREAD				BIT_ULL(2)
#define FIXED_0_ENABLE_PMI				BIT_ULL(3)

#define fixed_bits_by_idx(_idx, _bits)			\
	((_bits) << ((_idx) * FIXED_BITS_STRIDE))

#define AMD64_NR_COUNTERS				4
#define AMD64_NR_COUNTERS_CORE				6

#define PMU_CAP_FW_WRITES				BIT_ULL(13)
#define PMU_CAP_LBR_FMT				0x3f

enum intel_pmu_architectural_events {
	/*
	 * The order of the architectural events matters as support for each
	 * event is enumerated via CPUID using the index of the event.
	 */
	INTEL_ARCH_CPU_CYCLES,
	INTEL_ARCH_INSTRUCTIONS_RETIRED,
	INTEL_ARCH_REFERENCE_CYCLES,
	INTEL_ARCH_LLC_REFERENCES,
	INTEL_ARCH_LLC_MISSES,
	INTEL_ARCH_BRANCHES_RETIRED,
	INTEL_ARCH_BRANCHES_MISPREDICTED,
	NR_INTEL_ARCH_EVENTS,
};

enum amd_pmu_k7_events {
	AMD_ZEN_CORE_CYCLES,
	AMD_ZEN_INSTRUCTIONS,
	AMD_ZEN_BRANCHES,
	AMD_ZEN_BRANCH_MISSES,
	NR_AMD_ARCH_EVENTS,
};

extern const uint64_t intel_pmu_arch_events[];
extern const uint64_t amd_pmu_arch_events[];
extern const int intel_pmu_fixed_pmc_events[];

#endif /* SELFTEST_KVM_PMU_H */
