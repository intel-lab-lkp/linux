/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 Ventana Micro Systems Inc.
 */

#ifndef __RISCV_KVM_VMID_H_
#define __RISCV_KVM_VMID_H_

#include <linux/atomic.h>
#include <linux/kvm_types.h>

struct kvm_vmid {
	/*
	 * Software VMID:
	 *
	 *   [ generation | hardware VMID ]
	 *
	 * Keep the identity 64-bit on RV32 so that the generation does not
	 * wrap at the native word size.  Zero denotes an unallocated VMID.
	 *
	 * Only the low hardware VMID bits may be written to HGATP
	 * or used as a hardware fence VMID.
	 */
	atomic64_t id;
};

void __init kvm_riscv_gstage_vmid_detect(void);
unsigned long kvm_riscv_gstage_vmid_bits(void);

int __init kvm_riscv_gstage_vmid_alloc_init(void);
void kvm_riscv_gstage_vmid_alloc_free(void);

int kvm_riscv_gstage_vmid_init(struct kvm *kvm);

unsigned long kvm_riscv_gstage_vmid_hwid(u64 vmid);

bool kvm_riscv_gstage_vmid_ver_changed(struct kvm_vmid *vmid);

void kvm_riscv_gstage_vmid_update(struct kvm_vcpu *vcpu);

/*
 * Invalidate the current CPU's fast-path VMID state while preserving
 * the old identity conservatively in reserved_vmids.
 */
void kvm_riscv_gstage_vmid_cpu_invalidate(void);

#endif
