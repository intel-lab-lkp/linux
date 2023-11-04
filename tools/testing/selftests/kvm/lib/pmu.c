// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023, Tencent, Inc.
 */

#include <stdint.h>

#include "pmu.h"

/* Definitions for Architectural Performance Events */
#define ARCH_EVENT(select, umask) (((select) & 0xff) | ((umask) & 0xff) << 8)

const uint64_t intel_pmu_arch_events[] = {
	[INTEL_ARCH_CPU_CYCLES]			= ARCH_EVENT(0x3c, 0x0),
	[INTEL_ARCH_INSTRUCTIONS_RETIRED]	= ARCH_EVENT(0xc0, 0x0),
	[INTEL_ARCH_REFERENCE_CYCLES]		= ARCH_EVENT(0x3c, 0x1),
	[INTEL_ARCH_LLC_REFERENCES]		= ARCH_EVENT(0x2e, 0x4f),
	[INTEL_ARCH_LLC_MISSES]			= ARCH_EVENT(0x2e, 0x41),
	[INTEL_ARCH_BRANCHES_RETIRED]		= ARCH_EVENT(0xc4, 0x0),
	[INTEL_ARCH_BRANCHES_MISPREDICTED]	= ARCH_EVENT(0xc5, 0x0),
};

const uint64_t amd_pmu_arch_events[] = {
	[AMD_ZEN_CORE_CYCLES]			= ARCH_EVENT(0x76, 0x00),
	[AMD_ZEN_INSTRUCTIONS]			= ARCH_EVENT(0xc0, 0x00),
	[AMD_ZEN_BRANCHES]			= ARCH_EVENT(0xc2, 0x00),
	[AMD_ZEN_BRANCH_MISSES]			= ARCH_EVENT(0xc3, 0x00),
};
