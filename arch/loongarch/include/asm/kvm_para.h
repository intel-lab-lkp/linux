/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LOONGARCH_KVM_PARA_H
#define _ASM_LOONGARCH_KVM_PARA_H

/*
 * Hypcall code field
 */
#define HYPERVISOR_KVM			1
#define HYPERVISOR_VENDOR_SHIFT		8
#define HYPERCALL_CODE(vendor, code)	((vendor << HYPERVISOR_VENDOR_SHIFT) + code)

/*
 * LoongArch hypcall return code
 */
#define KVM_HC_STATUS_SUCCESS		0
#define KVM_HC_INVALID_CODE		-1UL
#define KVM_HC_INVALID_PARAMETER	-2UL

static inline unsigned int kvm_arch_para_features(void)
{
	return 0;
}

static inline unsigned int kvm_arch_para_hints(void)
{
	return 0;
}

static inline bool kvm_check_and_clear_guest_paused(void)
{
	return false;
}
#endif /* _ASM_LOONGARCH_KVM_PARA_H */
