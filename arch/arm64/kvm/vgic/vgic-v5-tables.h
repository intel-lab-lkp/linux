/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025, 2026 Arm Ltd.
 */

#ifndef __KVM_ARM_VGICV5_TABLES_H__
#define __KVM_ARM_VGICV5_TABLES_H__

#include <linux/idr.h>
#include <linux/irqchip/arm-gic-v5.h>

/* Level 1 Virtual Machine Table Entry */
typedef __le64 vmtl1_entry;

/* Level 2 Virtual Machine Table Entry */
struct vmtl2_entry {
	__le64 val[4];
};

/* Virtual PE Table Entry */
typedef __le64 vpe_entry;

struct vgic_v5_vm_info {
	void __iomem		*vmd_base;
	vpe_entry __iomem	*vpet_base;
	void __iomem		**vped_ptrs;
	u8			vpe_id_bits;
};

struct vgic_v5_vmt {
	union {
		struct {
			struct vmtl2_entry *vmt_base;
			unsigned int num_ents;
		} linear;
		struct {
			vmtl1_entry *vmt_base;
			struct vmtl2_entry **l2ptrs;
			unsigned int num_l1_ents;
		} l2;
	};
	bool		two_level;
	unsigned int	num_entries;
	unsigned int	max_vpes;
	size_t		vmd_size;
	size_t		vped_size;
	struct ida	vm_id_ida;
};

static inline u32 vgic_v5_vm_id(struct kvm *kvm)
{
	return kvm->arch.vgic.gicv5_vm.vm_id;
}

static inline u16 vgic_v5_vpe_id(struct kvm_vcpu *vcpu)
{
	return vcpu->vcpu_idx;
}

static inline int vgic_v5_vpe_db(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.vgic_cpu.vgic_v5.gicv5_vpe.db;
}

int vgic_v5_vmt_allocate(unsigned int max_vpes);
int vgic_v5_vmt_free(void);

int vgic_v5_allocate_vm_id(struct kvm *kvm);
void vgic_v5_release_vm_id(struct kvm *kvm);

int vgic_v5_vmte_init(struct kvm *kvm);
int vgic_v5_vmte_release(struct kvm *kvm);
int vgic_v5_vmte_alloc_vpe(struct kvm_vcpu *vcpu);
int vgic_v5_vmte_free_vpe(struct kvm_vcpu *vcpu);

#endif
