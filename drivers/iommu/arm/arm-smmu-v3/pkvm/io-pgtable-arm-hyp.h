/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IO_PGTABLE_ARM_HYP_H_
#define IO_PGTABLE_ARM_HYP_H_

#include <nvhe/memory.h>
#include <nvhe/iommu.h>

#undef WARN_ONCE
#define WARN_ONCE(condition, format...) WARN_ON(condition)

static inline void *iommu_alloc_pages_node_sz(int nid, gfp_t gfp, size_t size)
{
	return pkvm_iommu_alloc_pages(get_order(size));
}

static inline void iommu_free_pages(void *virt)
{
	pkvm_iommu_free_pages(virt);
}

#define arm_lpae_alloc_data(type)	pkvm_iommu_alloc_pages(get_order(sizeof(type)))
#define arm_lpae_free_data(p)		pkvm_iommu_free_pages(p)

#undef phys_to_virt
#define phys_to_virt(x)		hyp_phys_to_virt(x)

#undef virt_to_phys
#define virt_to_phys(x)		hyp_virt_to_phys(x)

/* Stubs used by io-pgtable-arm but no relevant for hypervisor object. */
#undef dma_map_single
#define dma_map_single(d, p, s, dir) (virt_to_phys(p))

#undef dma_unmap_single
#define dma_unmap_single(d, a, s, r) \
	do { (void)(d); (void)(a); (void)(s); (void)(r); } while (0)

#define dma_sync_single_for_device(d, a, s, r) \
	do { (void)(d); (void)(a); (void)(s); (void)(r); } while (0)

#undef dma_mapping_error
#define dma_mapping_error(...) (0)

#undef dev_err
#define dev_err(dev, format, ...)

#undef iommu_dirty_bitmap_record
#define iommu_dirty_bitmap_record(d, i, l) \
	do { (void)(d); (void)(i); (void)(l); } while (0)

static inline int iommu_pages_start_incoherent(void *virt, struct device *dma_dev)
{
	return 0;
}

static inline void iommu_pages_free_incoherent(void *virt,
					       struct device *dma_dev)
{
}

static inline void iommu_pages_flush_incoherent(struct device *dma_dev,
						void *virt, size_t offset,
						size_t len)
{
}

static inline struct io_pgtable_ops *kvm_alloc_io_pgtable_ops(enum io_pgtable_fmt fmt,
							      struct io_pgtable_cfg *cfg,
							      void *cookie)
{
	struct io_pgtable *iop;

	if (fmt != ARM_64_LPAE_S2)
		return NULL;

	iop = io_pgtable_arm_64_lpae_s2_init_fns.alloc(cfg, cookie);
	if (!iop)
		return NULL;

	iop->fmt	= fmt;
	iop->cookie	= cookie;
	iop->cfg	= *cfg;

	return &iop->ops;
}
#endif /* IO_PGTABLE_ARM_HYP_H_ */
