/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Per-vCPU buffer management for HDBSS-based dirty page tracking.
 *
 * Copyright (C) 2026 Huawei Technologies Co., Ltd
 * Author: Tian Zheng <zhengtian10@huawei.com>
 */

#ifndef __ARM64_KVM_DIRTY_BIT_H__
#define __ARM64_KVM_DIRTY_BIT_H__

#include <asm/kvm_pgtable.h>
#include <asm/sysreg.h>
#include <linux/sizes.h>

#define HDBSS_ENTRY_VALID	BIT(0)
#define HDBSS_ENTRY_IPA	GENMASK_ULL(55, 12)

#define KVM_ARM_HDBSS_DEFAULT_SIZE  PAGE_SIZE
#define KVM_ARM_HDBSS_MAX_SIZE      SZ_2M

/* 0 means unconfigured, fall back to one page per vCPU. */
static inline u32 kvm_hdbss_buffer_size(struct kvm *kvm)
{
	return kvm->arch.hdbss_buffer_size ?: KVM_ARM_HDBSS_DEFAULT_SIZE;
}

static inline bool kvm_hdbss_enabled(struct kvm *kvm)
{
	return kvm->arch.mmu.vtcr & VTCR_EL2_HDBSS;
}

static inline bool vcpu_hdbss_enabled(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.hw_mmu &&
		(vcpu->arch.hw_mmu->vtcr & VTCR_EL2_HDBSS);
}

int kvm_arm_vcpu_alloc_hdbss(struct kvm_vcpu *vcpu);
void kvm_arm_vcpu_free_hdbss(struct kvm_vcpu *vcpu);
void kvm_flush_hdbss_buffer(struct kvm_vcpu *vcpu);
int kvm_handle_hdbss_fault(struct kvm_vcpu *vcpu);

#endif /* __ARM64_KVM_DIRTY_BIT_H__ */
