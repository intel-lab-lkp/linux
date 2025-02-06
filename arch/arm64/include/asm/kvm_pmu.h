/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __KVM_PMU_H
#define __KVM_PMU_H

void kvm_vcpu_pmu_resync_el0(void);

#ifdef CONFIG_KVM
void kvm_set_pmu_events(u64 set, struct perf_event_attr *attr);
void kvm_clr_pmu_events(u64 clr);
bool kvm_set_pmuserenr(u64 val);
#else
static inline void kvm_set_pmu_events(u64 set, struct perf_event_attr *attr) {}
static inline void kvm_clr_pmu_events(u64 clr) {}
static inline bool kvm_set_pmuserenr(u64 val)
{
	return false;
}
#endif

static inline bool kvm_pmu_counter_deferred(struct perf_event_attr *attr)
{
	return (!has_vhe() && attr->exclude_host);
}

#endif
