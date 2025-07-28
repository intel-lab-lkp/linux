// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Arm Ltd.
 */
#include <nvhe/iommu.h>

#include "../../../io-pgtable-arm.h"

void arm_lpae_split_blk(void)
{
	WARN_ON(1);
}

void *__arm_lpae_alloc_pages(size_t size, gfp_t gfp,
			     struct io_pgtable_cfg *cfg, void *cookie)
{
	void *addr;

	if (!PAGE_ALIGNED(size))
		return NULL;

	addr = kvm_iommu_donate_pages(get_order(size));

	if (addr && !cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	return addr;
}

void __arm_lpae_free_pages(void *addr, size_t size, struct io_pgtable_cfg *cfg,
			   void *cookie)
{
	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(addr, size);

	kvm_iommu_reclaim_pages(addr);
}

void __arm_lpae_sync_pte(arm_lpae_iopte *ptep, int num_entries,
			 struct io_pgtable_cfg *cfg)
{
	if (!cfg->coherent_walk)
		kvm_flush_dcache_to_poc(ptep, sizeof(*ptep) * num_entries);
}

static int kvm_arm_io_pgtable_init(struct io_pgtable_cfg *cfg,
				   enum io_pgtable_fmt fmt,
				   struct arm_lpae_io_pgtable *data,
				   void *cookie)
{
	int ret = -EINVAL;

	if (fmt == ARM_64_LPAE_S2)
		ret = arm_lpae_init_pgtable_s2(cfg, data, cookie);
	else if (fmt == ARM_64_LPAE_S1)
		ret = arm_lpae_init_pgtable_s1(cfg, data, cookie);

	if (ret)
		return ret;

	data->iop.cfg = *cfg;
	data->iop.fmt	= fmt;
	return 0;
}

struct io_pgtable *kvm_arm_io_pgtable_alloc(struct io_pgtable_cfg *cfg,
					    void *cookie,
					    enum io_pgtable_fmt fmt,
					    int *out_ret)
{
	size_t pgd_size;
	struct arm_lpae_io_pgtable *data;
	int ret;

	data = kvm_iommu_donate_pages(get_order(sizeof(*data)));
	if (!data) {
		*out_ret = -ENOMEM;
		return NULL;
	}

	data->iop.ops = (struct io_pgtable_ops) {
		.map_pages	= arm_lpae_map_pages,
		.unmap_pages	= arm_lpae_unmap_pages,
	};

	ret = kvm_arm_io_pgtable_init(cfg, fmt, data, cookie);
	if (ret) {
		*out_ret = ret;
		goto out_free;
	}
	pgd_size = PAGE_ALIGN(ARM_LPAE_PGD_SIZE(data));
	data->pgd = __arm_lpae_alloc_pages(pgd_size, 0, &data->iop.cfg, cookie);
	if (!data->pgd) {
		ret = -ENOMEM;
		goto out_free;
	}

	if (fmt == ARM_64_LPAE_S2)
		data->iop.cfg.arm_lpae_s2_cfg.vttbr = __arm_lpae_virt_to_phys(data->pgd);
	else if (fmt == ARM_64_LPAE_S1)
		data->iop.cfg.arm_lpae_s1_cfg.ttbr = __arm_lpae_virt_to_phys(data->pgd);

	if (!data->iop.cfg.coherent_walk)
		kvm_flush_dcache_to_poc(data->pgd, pgd_size);

	/* Ensure the empty pgd is visible before any actual TTBR write */
	wmb();

	*out_ret = 0;
	return &data->iop;
out_free:
	kvm_iommu_reclaim_pages(data);
	*out_ret = ret;
	return NULL;
}
