// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Google LLC
 * Author: Colton Lewis <coltonlewis@google.com>
 */

#include <linux/kvm_host.h>
#include <linux/perf/arm_pmu.h>

#include <asm/kvm_pmu.h>

static u8 reserved_host_counters __read_mostly;

module_param(reserved_host_counters, byte, 0);
MODULE_PARM_DESC(reserved_host_counters,
		 "Partition the PMU into host and guest counters");

u8 kvm_pmu_get_reserved_counters(void)
{
	return reserved_host_counters;
}

u8 kvm_pmu_hpmn(u8 nr_counters)
{
	if (reserved_host_counters >= nr_counters) {
		if (this_cpu_has_cap(ARM64_HAS_HPMN0))
			return 0;

		return 1;
	}

	return nr_counters - reserved_host_counters;
}

void kvm_pmu_partition(struct arm_pmu *pmu)
{
	u8 nr_counters = *host_data_ptr(nr_event_counters);
	u8 hpmn = kvm_pmu_hpmn(nr_counters);

	if (hpmn < nr_counters) {
		pmu->hpmn = hpmn;
		pmu->partitioned = true;
	} else {
		pmu->hpmn = nr_counters;
		pmu->partitioned = false;
	}
}
