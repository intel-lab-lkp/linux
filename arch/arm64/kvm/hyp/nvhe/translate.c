// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2025 Amazon.com, Inc. or its affiliates. All rights reserved.
 * Author: Priscilla Lam <prl@amazon.com>
 */

#include <asm/sysreg.h>
#include <hyp/sysreg-sr.h>
#include <nvhe/mem_protect.h>

static __always_inline u64 par_to_ipa(u64 par, u64 va)
{
	u64 offset = va & ((1ULL << PAGE_SHIFT) - 1);

	return (par & GENMASK_ULL(51, 12)) | offset;
}

/**
 * __kvm_hyp_translate - hypercall that translates a GVA to GPA when VHE is not enabled or available
 * @vcpu: the vCPU pointer
 * @gva: the guest virtual address
 *
 * This returns the result in a packed integer. The GPA if successful will be in bits 63:8, the
 * validity in bit 4, and if the address is writeable in bit 0.
 */
u64 __kvm_hyp_translate(struct kvm_vcpu *vcpu, u64 gva)
{
	struct kvm_cpu_context *host_ctxt;
	struct kvm_cpu_context *guest_ctxt;
	struct kvm_s2_mmu *mmu;

	u64 hcr_old = read_sysreg(hcr_el2);
	u64 par = 0;
	u64 gpa = 0;
	bool valid = false;
	bool writeable = false;

	host_ctxt = host_data_ptr(host_ctxt);
	host_ctxt->__hyp_running_vcpu = vcpu;
	guest_ctxt = &vcpu->arch.ctxt;

	__sysreg_save_state_nvhe(host_ctxt);
	__debug_save_host_buffers_nvhe(vcpu);

	dsb(nsh);

	__sysreg_restore_state_nvhe(guest_ctxt);

	mmu = kern_hyp_va(vcpu->arch.hw_mmu);
	__load_stage2(mmu, kern_hyp_va(mmu->arch));

	write_sysreg((hcr_old | HCR_E2H | HCR_VM) & ~HCR_TGE, hcr_el2);
	isb();

	asm volatile("at s1e1r, %0" :: "r"(gva));
	isb();

	par = read_sysreg(par_el1);

	if (!(par & 1)) {
		gpa = par_to_ipa(par, gva);
		valid = true;
	}

	if (valid) {
		asm volatile("at s1e1w, %0" :: "r"(gva));
		isb();

		par = read_sysreg(par_el1);
		if (!(par & 1))
			writeable = true;
	}

	write_sysreg(hcr_old, hcr_el2);
	isb();

	__load_host_stage2();
	__sysreg_restore_state_nvhe(host_ctxt);
	__debug_restore_host_buffers_nvhe(vcpu);
	host_ctxt->__hyp_running_vcpu = NULL;

	// Pack result: GPA in bits 63:8, valid in bit 4, writeable in bit 0
	return (gpa << 8) | (valid ? (1ULL << 4) : 0) | (writeable ? 1ULL : 0);
}
