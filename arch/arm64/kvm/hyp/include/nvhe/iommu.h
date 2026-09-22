/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

#include <asm/kvm_pgtable.h>

struct pkvm_iommu_ops {
	int (*init)(void);
	int (*host_stage2_idmap)(phys_addr_t start, phys_addr_t end, int prot);
};

int pkvm_iommu_init(void *pool_base, unsigned int nr_pages);

int pkvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot);

/* Allocate pages from the IOMMU carveout, returns zeroed memory. */
void *pkvm_iommu_alloc_pages(u8 order);
/* Free pages from pkvm_iommu_alloc_pages(). */
void pkvm_iommu_free_pages(void *ptr);

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
