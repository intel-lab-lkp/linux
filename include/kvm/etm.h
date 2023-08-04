/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __KVM_DEBUG_H
#define __KVM_DEBUG_H

struct perf_event_attr;
struct kvm_vcpu;

#if IS_ENABLED(CONFIG_KVM) && IS_ENABLED(CONFIG_PERF_EVENTS)

struct kvm_etm_event {
	bool exclude_host;
	bool exclude_guest;
	bool exclude_kernel;
	bool exclude_user;
};

struct kvm_etm_event *kvm_get_etm_event(void);
void kvm_etm_clr_events(void);
void kvm_etm_set_events(struct perf_event_attr *attr);

/*
 * Updates the vcpu's view of the etm events for this cpu. Must be
 * called before every vcpu run after disabling interrupts, to ensure
 * that an interrupt cannot fire and update the structure.
 */
#define kvm_etm_update_vcpu_events(vcpu)						\
	do {										\
		if (!has_vhe() && vcpu_get_flag(vcpu, DEBUG_STATE_SAVE_TRFCR))		\
			vcpu->arch.host_debug_state.etm_event = *kvm_get_etm_event();	\
	} while (0)

#else

struct kvm_etm_event {};

static inline void kvm_etm_update_vcpu_events(struct kvm_vcpu *vcpu) {}
static inline void kvm_etm_set_events(struct perf_event_attr *attr) {}
static inline void kvm_etm_clr_events(void) {}

#endif

#endif
