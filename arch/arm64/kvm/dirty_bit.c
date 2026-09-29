// SPDX-License-Identifier: GPL-2.0-only
/*
 * Per-vCPU HDBSS buffer management.
 *
 * Copyright (C) 2026 Huawei Technologies Co., Ltd
 * Author: Tian Zheng <zhengtian10@huawei.com>
 */

#include <asm/kvm_dirty_bit.h>
#include <asm/kvm_mmu.h>
#include <asm/sysreg.h>
#include <linux/gfp.h>
#include <linux/kconfig.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/srcu.h>

int kvm_arm_vcpu_alloc_hdbss(struct kvm_vcpu *vcpu)
{
	struct page *hdbss_pg;
	u32 size;
	unsigned int buddy_order;
	u32 sz_encoded;

	if (vcpu->arch.hdbss.hdbss_pg || !system_supports_hdbss())
		return 0;

	size = kvm_hdbss_buffer_size(vcpu->kvm);

	buddy_order = get_order(size);
	sz_encoded = ilog2(size) - 12;

	hdbss_pg = alloc_pages(GFP_KERNEL_ACCOUNT | __GFP_ZERO, buddy_order);
	if (!hdbss_pg)
		return -ENOMEM;

	vcpu->arch.hdbss = (struct vcpu_hdbss_state) {
		.hdbss_pg = hdbss_pg,
		.hdbssbr_el2 = HDBSSBR_EL2(page_to_phys(hdbss_pg), sz_encoded),
		.hdbssprod_el2 = 0,
		.buddy_order = buddy_order,
	};

	return 0;
}

void kvm_arm_vcpu_free_hdbss(struct kvm_vcpu *vcpu)
{
	if (!vcpu->arch.hdbss.hdbss_pg)
		return;

	__free_pages(vcpu->arch.hdbss.hdbss_pg, vcpu->arch.hdbss.buddy_order);

	vcpu->arch.hdbss.hdbss_pg = NULL;
	vcpu->arch.hdbss.hdbssbr_el2 = 0;
}

void kvm_flush_hdbss_buffer(struct kvm_vcpu *vcpu)
{
	int idx, curr_idx;
	u64 prod;
	u32 entries;
	u64 *hdbss_buf;
	struct kvm *kvm = vcpu->kvm;
	int srcu_idx;

	if (!vcpu_hdbss_enabled(vcpu))
		return;

	prod = read_sysreg_s(SYS_HDBSSPROD_EL2);
	curr_idx = HDBSSPROD_IDX(prod);

	if (curr_idx == 0 || !vcpu->arch.hdbss.hdbss_pg)
		return;

	hdbss_buf = page_address(vcpu->arch.hdbss.hdbss_pg);
	if (!hdbss_buf)
		return;

	entries = kvm_hdbss_buffer_size(kvm) / sizeof(u64);

	/* kvm_vcpu_mark_page_dirty() resolves the memslot under SRCU. */
	srcu_idx = srcu_read_lock(&kvm->srcu);
	for (idx = 0; idx < min_t(u32, curr_idx, entries); idx++) {
		u64 gpa;

		gpa = hdbss_buf[idx];
		if (!(gpa & HDBSS_ENTRY_VALID))
			continue;

		gpa &= HDBSS_ENTRY_IPA;
		kvm_vcpu_mark_page_dirty(vcpu, gpa >> PAGE_SHIFT);
	}
	srcu_read_unlock(&kvm->srcu, srcu_idx);

	prod &= ~HDBSSPROD_EL2_INDEX_MASK;
	write_sysreg_s(prod, SYS_HDBSSPROD_EL2);
	vcpu->arch.hdbss.hdbssprod_el2 = prod;
	isb();
}

int kvm_handle_hdbss_fault(struct kvm_vcpu *vcpu)
{
	u64 prod;
	u64 fsc;

	if (WARN_ON_ONCE(!system_supports_hdbss()))
		return -EFAULT;

	if (WARN_ON_ONCE(!vcpu_hdbss_enabled(vcpu)))
		return -EFAULT;

	prod = read_sysreg_s(SYS_HDBSSPROD_EL2);
	fsc = FIELD_GET(HDBSSPROD_EL2_FSC_MASK, prod);

	if (fsc == HDBSSPROD_EL2_FSC_OK)
		/* Buffer full: the exit path drained it before handle_exit. */
		return 1;

	if (fsc != HDBSSPROD_EL2_FSC_ExternalAbort &&
	    fsc != HDBSSPROD_EL2_FSC_GPF)
		WARN_ONCE(1,
			  "Unexpected HDBSS fault type, FSC: 0x%llx (prod=0x%llx, vcpu=%d)\n",
			  fsc, prod, vcpu->vcpu_id);

	/* Clear FSC so hardware dirty state updates can resume. */
	write_sysreg_s(prod & ~HDBSSPROD_EL2_FSC_MASK, SYS_HDBSSPROD_EL2);
	return -EFAULT;
}
