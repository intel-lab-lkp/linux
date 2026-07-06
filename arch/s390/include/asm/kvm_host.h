/* SPDX-License-Identifier: GPL-2.0 */

#ifndef ASM_KVM_HOST_H
#define ASM_KVM_HOST_H

#ifdef KVM_S390_ARM64
#include <asm/kvm_host_arm64.h>
#else
#include <asm/kvm_host_s390.h>
#endif

static inline  bool kvm_arch_pmi_in_guest(struct kvm_vcpu *vcpu)
{
	return false;
}

#define PGM_PROTECTION			0x04
#define PGM_ADDRESSING			0x05
#define PGM_SEGMENT_TRANSLATION		0x10
#define PGM_PAGE_TRANSLATION		0x11
#define PGM_ASCE_TYPE			0x38
#define PGM_REGION_FIRST_TRANS		0x39
#define PGM_REGION_SECOND_TRANS		0x3a
#define PGM_REGION_THIRD_TRANS		0x3b

#endif /* ASM_KVM_HOST_H */
