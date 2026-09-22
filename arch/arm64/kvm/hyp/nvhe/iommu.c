// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <linux/iommu.h>
#include <asm/kvm_pkvm.h>

#include <nvhe/iommu.h>
#include <nvhe/mem_protect.h>
#include <nvhe/spinlock.h>

/* Only one set of ops supported */
struct pkvm_iommu_ops *pkvm_iommu_ops;

/* Protected by host_mmu.lock */
static bool pkvm_idmap_initialized;
static struct hyp_pool iommu_pages_pool;

static inline int pkvm_to_iommu_prot(enum kvm_pgtable_prot prot)
{
	int iommu_prot = 0;

	if (prot & KVM_PGTABLE_PROT_R)
		iommu_prot |= IOMMU_READ;
	if (prot & KVM_PGTABLE_PROT_W)
		iommu_prot |= IOMMU_WRITE;

	/* We don't understand that, might be dangerous. */
	WARN_ON(prot & ~PKVM_HOST_MEM_PROT);
	return iommu_prot;
}

/*
 * IOMMU page tables are shadowed and not shared, that is mainly because:
 * - Possible inconsistency between IOMMU and CPU features or format.
 * - KVM relies on handling in page faults (BBM, lazy mapping).
 */
static int __snapshot_host_stage2(const struct kvm_pgtable_visit_ctx *ctx,
				  enum kvm_pgtable_walk_flags visit)
{
	bool is_memory = *(bool *)ctx->arg;
	kvm_pte_t pte = *ctx->ptep;
	u64 start = ctx->addr;
	u64 block_end, end;
	int prot;

	block_end = ALIGN_DOWN(ctx->addr, kvm_granule_size(ctx->level)) +
			kvm_granule_size(ctx->level);
	end = min(ctx->end, block_end);
	/*
	 * Keep annotated PTEs unmapped, and map everything else even lazily
	 * mapped PTEs (0), as the IOMMU can't handle page faults.
	 * That maps the whole address space which can be large, but that doesn't
	 * use a lot of memory as it will be mostly large blocks (1 GB with 4kb pages)
	 */
	if (pte && !kvm_pte_valid(pte))
		return 0;

	if (kvm_pte_valid(pte))
		prot = pkvm_to_iommu_prot(kvm_pgtable_stage2_pte_prot(pte));
	else
		prot = IOMMU_READ | IOMMU_WRITE;

	if (!is_memory)
		prot |= IOMMU_MMIO;

	return pkvm_iommu_ops->host_stage2_idmap(start, end, prot);
}

static int pkvm_iommu_snapshot_host_stage2(void)
{
	struct kvm_pgtable *pgt = &host_mmu.pgt;
	bool is_memory;
	struct kvm_pgtable_walker walker = {
		.cb	= __snapshot_host_stage2,
		.flags	= KVM_PGTABLE_WALK_LEAF,
		.arg	= &is_memory,
	};
	int ret = 0, i;
	u64 start = 0;

	hyp_spin_lock(&host_mmu.lock);
	for (i = 0; i < hyp_memblock_nr; i++) {
		struct memblock_region *reg = &hyp_memory[i];

		if (start < reg->base) {
			is_memory = false;
			ret = kvm_pgtable_walk(pgt, start, reg->base - start, &walker);
			if (ret)
				goto out_unlock;
		}

		is_memory = true;
		ret = kvm_pgtable_walk(pgt, reg->base, reg->size, &walker);
		if (ret)
			goto out_unlock;

		start = reg->base + reg->size;
	}

	if (start < BIT(pgt->ia_bits)) {
		is_memory = false;
		ret = kvm_pgtable_walk(pgt, start, BIT(pgt->ia_bits) - start, &walker);
		if (ret)
			goto out_unlock;
	}

	pkvm_idmap_initialized = true;

out_unlock:
	hyp_spin_unlock(&host_mmu.lock);
	return ret;
}

int pkvm_iommu_init(void *pool_base, unsigned int nr_pages)
{
	int ret;

	/* Keep DMA isolation optional. */
	if (!pkvm_iommu_ops || !pkvm_iommu_ops->init ||
	    !pkvm_iommu_ops->host_stage2_idmap)
		return 0;

	if (!nr_pages)
		return -ENOMEM;

	ret = hyp_pool_init(&iommu_pages_pool, hyp_virt_to_pfn(pool_base),
			    nr_pages, 0);
	if (ret)
		return ret;

	ret = pkvm_iommu_ops->init();
	if (ret)
		return ret;

	return WARN_ON(pkvm_iommu_snapshot_host_stage2());
}

int pkvm_iommu_host_stage2_idmap(phys_addr_t start, phys_addr_t end,
				 enum kvm_pgtable_prot prot)
{
	hyp_assert_lock_held(&host_mmu.lock);

	if (!pkvm_idmap_initialized)
		return 0;

	return pkvm_iommu_ops->host_stage2_idmap(start, end, pkvm_to_iommu_prot(prot));
}

void *pkvm_iommu_alloc_pages(u8 order)
{
	return hyp_alloc_pages(&iommu_pages_pool, order);
}

void pkvm_iommu_free_pages(void *ptr)
{
	hyp_put_page(&iommu_pages_pool, ptr);
}
