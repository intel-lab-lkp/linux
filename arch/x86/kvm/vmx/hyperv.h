/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_X86_VMX_HYPERV_H
#define __KVM_X86_VMX_HYPERV_H

#include <linux/kvm_host.h>
#include "vmcs12.h"
#include "vmx.h"

#define EVMPTR_INVALID (-1ULL)
#define EVMPTR_MAP_PENDING (-2ULL)

static inline bool evmptr_is_valid(u64 evmptr)
{
	return evmptr != EVMPTR_INVALID && evmptr != EVMPTR_MAP_PENDING;
}

enum nested_evmptrld_status {
	EVMPTRLD_DISABLED,
	EVMPTRLD_SUCCEEDED,
	EVMPTRLD_VMFAIL,
	EVMPTRLD_ERROR,
};

struct vcpu_vmx;

#ifdef CONFIG_KVM_HYPERV
static inline gpa_t nested_vmx_evmptr(struct vcpu_vmx *vmx) { return vmx->nested.hv_evmcs_vmptr; }
static inline struct hv_enlightened_vmcs *nested_vmx_evmcs(struct vcpu_vmx *vmx)
{
	return vmx->nested.hv_evmcs;
}
u64 nested_get_evmptr(struct kvm_vcpu *vcpu);
uint16_t nested_get_evmcs_version(struct kvm_vcpu *vcpu);
int nested_enable_evmcs(struct kvm_vcpu *vcpu,
			uint16_t *vmcs_version);
void nested_evmcs_filter_control_msr(struct kvm_vcpu *vcpu, u32 msr_index, u64 *pdata);
int nested_evmcs_check_controls(struct vmcs12 *vmcs12);
bool nested_evmcs_l2_tlb_flush_enabled(struct kvm_vcpu *vcpu);
void vmx_hv_inject_synthetic_vmexit_post_tlb_flush(struct kvm_vcpu *vcpu);
#else
static inline gpa_t nested_vmx_evmptr(struct vcpu_vmx *vmx) { return EVMPTR_INVALID; };
static inline struct hv_enlightened_vmcs *nested_vmx_evmcs(struct vcpu_vmx *vmx)
{
	return NULL;
}
static inline u64 nested_get_evmptr(struct kvm_vcpu *vcpu) { return EVMPTR_INVALID; }
static inline void nested_evmcs_filter_control_msr(struct kvm_vcpu *vcpu, u32 msr_index, u64 *pdata) {}
static inline bool nested_evmcs_l2_tlb_flush_enabled(struct kvm_vcpu *vcpu) { return false; }
static inline int nested_evmcs_check_controls(struct vmcs12 *vmcs12) { return 0; }
#endif


#endif /* __KVM_X86_VMX_HYPERV_H */
