/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __KVM_PMU_H
#define __KVM_PMU_H

/*
 * Define the interface between the PMUv3 driver and KVM.
 */
struct perf_event_attr;
struct arm_pmu;

#define kvm_pmu_counter_deferred(attr)			\
	({						\
		!has_vhe() && (attr)->exclude_host;	\
	})

#ifdef CONFIG_KVM

void kvm_set_pmu_events(u64 set, struct perf_event_attr *attr);
void kvm_clr_pmu_events(u64 clr);
bool kvm_set_pmuserenr(u64 val);
void kvm_vcpu_pmu_resync_el0(void);
void kvm_host_pmu_init(struct arm_pmu *pmu);

u8 kvm_pmu_get_reserved_counters(void);
u8 kvm_pmu_hpmn(u8 nr_counters);
void kvm_pmu_partition(struct arm_pmu *pmu);
void kvm_pmu_host_counters_enable(void);
void kvm_pmu_host_counters_disable(void);

#else

static inline void kvm_set_pmu_events(u64 set, struct perf_event_attr *attr) {}
static inline void kvm_clr_pmu_events(u64 clr) {}
static inline bool kvm_set_pmuserenr(u64 val)
{
	return false;
}
static inline void kvm_vcpu_pmu_resync_el0(void) {}
static inline void kvm_host_pmu_init(struct arm_pmu *pmu) {}

static inline void kvm_pmu_host_counters_enable(void) {}
static inline void kvm_pmu_host_counters_disable(void) {}

#endif

#endif
