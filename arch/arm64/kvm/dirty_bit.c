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
