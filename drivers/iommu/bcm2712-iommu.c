// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU driver for Broadcom BCM2712
 *
 * Copyright (c) 2023-2025 Raspberry Pi Ltd.
 * Copyright (c) 2026 Daniel Drake
 *
 * Each BCM2712 IOMMU has multiple devices hardwired into it, whose
 * DMA transactions all route through the IOMMU. There is no stream ID tagging
 * or any other kind of segmentation to differentiate between requests from
 * different devices. It is also not possible to toggle a specific device
 * between iommu-mapped and bypass modes.
 *
 * With these limitations in mind, we configure the hardware to operate over two
 * windows:
 *  1. The lower part of the address space operates in identity/bypass mode.
 *     This enables non-IOMMU-consumer devices to work as normal (including
 *     devices set up by the firmware, devices that don't require large DMA
 *     allocations, devices that handle scatter-gather natively, etc.)
 *  2. A 4GB translation aperture is defined at a fixed, high address, beyond
 *     regular RAM and MMIO space. Mappings are operated in this aperture for
 *     devices that wish to take advantage of the IOMMU.
 *
 * The BCM2712 SoC contains multiple independent IOMMU hardware instances.
 * The same aperture address is used across all of them; they do not conflict
 * because these IOVA spaces are strictly localized to each IOMMU instance.
 *
 * The page table format is a two-level format handled by generic_pt/bcm2712.
 */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/sizes.h>
#include <linux/generic_pt/iommu.h>

#include "iommu-pages.h"
#include "bcm2712-iommu.h"

/* BCM2712 IOMMU is organized around 4Kbyte pages */
#define IOMMU_PAGE_SHIFT       12
#define IOMMU_PAGE_SIZE        (1ul << IOMMU_PAGE_SHIFT)
/* A PTE is 4 bytes */
#define PTE_SIZE_SHIFT         2
/* L1/L2 table sizing (IOMMU hardware pages): 1024 entries per page */
#define PTES_PER_IOPG_SHIFT    (IOMMU_PAGE_SHIFT - PTE_SIZE_SHIFT)
/* An iommu hugepage covers 4MB  */
#define IOMMU_HUGEPAGE_SHIFT   (IOMMU_PAGE_SHIFT + PTES_PER_IOPG_SHIFT)


#define MMMU_CTRL_OFFSET                       0x00
#define MMMU_CTRL_CAP_EXCEEDED                 BIT(27)
#define MMMU_CTRL_CAP_EXCEEDED_ABORT_EN        BIT(26)
#define MMMU_CTRL_CAP_EXCEEDED_INT_EN          BIT(25)
#define MMMU_CTRL_CAP_EXCEEDED_EXCEPTION_EN    BIT(24)
#define MMMU_CTRL_PT_INVALID                   BIT(20)
#define MMMU_CTRL_PT_INVALID_ABORT_EN          BIT(19)
#define MMMU_CTRL_PT_INVALID_INT_EN            BIT(18)
#define MMMU_CTRL_PT_INVALID_EXCEPTION_EN      BIT(17)
#define MMMU_CTRL_PT_INVALID_EN                BIT(16)
#define MMMU_CTRL_WRITE_VIOLATION              BIT(12)
#define MMMU_CTRL_WRITE_VIOLATION_ABORT_EN     BIT(11)
#define MMMU_CTRL_WRITE_VIOLATION_INT_EN       BIT(10)
#define MMMU_CTRL_WRITE_VIOLATION_EXCEPTION_EN BIT(9)
#define MMMU_CTRL_BYPASS                       BIT(8)
#define MMMU_CTRL_TLB_CLEARING                 BIT(7)
#define MMMU_CTRL_STATS_CLEAR                  BIT(3)
#define MMMU_CTRL_TLB_CLEAR                    BIT(2)
#define MMMU_CTRL_STATS_ENABLE                 BIT(1)
#define MMMU_CTRL_ENABLE                       BIT(0)

#define MMMU_CTRL_OPERATING_FLAGS (\
	MMMU_CTRL_CAP_EXCEEDED_ABORT_EN    | \
	MMMU_CTRL_PT_INVALID_ABORT_EN      | \
	MMMU_CTRL_PT_INVALID_EN            | \
	MMMU_CTRL_WRITE_VIOLATION_ABORT_EN | \
	MMMU_CTRL_STATS_ENABLE             | \
	MMMU_CTRL_ENABLE)

#define MMMU_PT_PA_BASE_OFFSET                 0x04

#define MMMU_ADDR_CAP_OFFSET                   0x14
#define MMMU_ADDR_CAP_ENABLE                   BIT(31)
#define ADDR_CAP_SHIFT                         ilog2(SZ_256M)

#define MMMU_SHOOT_DOWN_OFFSET                 0x18
#define MMMU_SHOOT_DOWN_SHOOTING               BIT(31)
#define MMMU_SHOOT_DOWN_SHOOT                  BIT(30)

#define MMMU_BYPASS_START_OFFSET               0x1c
#define MMMU_BYPASS_START_ENABLE               BIT(31)

#define MMMU_BYPASS_END_OFFSET                 0x20
#define MMMU_BYPASS_END_ENABLE                 BIT(31)

#define MMMU_MISC_OFFSET                       0x24
#define MMMU_MISC_SINGLE_TABLE                 BIT(31)

#define MMMU_ILLEGAL_ADR_OFFSET                0x30
#define MMMU_ILLEGAL_ADR_ENABLE                BIT(31)

#define MMMU_DEBUG_INFO_OFFSET                 0x38
#define MMMU_DEBUG_INFO_VERSION_MASK           0x0000000Fu
#define MMMU_DEBUG_INFO_VA_WIDTH_MASK          0x000000F0u
#define MMMU_DEBUG_INFO_PA_WIDTH_MASK          0x00000F00u
#define MMMU_DEBUG_INFO_BIGPAGE_WIDTH_MASK     0x000FF000u
#define MMMU_DEBUG_INFO_SUPERPAGE_WIDTH_MASK   0x0FF00000u
#define MMMU_DEBUG_INFO_BYPASS_4M              BIT(28)
#define MMMU_DEBUG_INFO_BYPASS                 BIT(29)

struct bcm2712_iommu {
	struct device *dev;
	struct iommu_device iommu;
	struct bcm2712_iommu_domain *domain;
	struct bcm2712_iommu_cache *cache;
	void __iomem *reg_base;
	spinlock_t hw_lock;
	size_t bigpage_size;
	size_t superpage_size;
};

struct bcm2712_iommu_domain {
	union {
		struct iommu_domain base;
		struct pt_iommu_bcm2712 pt;
	};
	struct bcm2712_iommu *mmu;
	void *default_page;
};

#define MMU_WR(off, val)  writel(val, mmu->reg_base + (off))
#define MMU_RD(off)       readl(mmu->reg_base + (off))

#define domain_to_mmu(d) \
	(container_of(d, struct bcm2712_iommu_domain, base)->mmu)

static struct bcm2712_iommu_domain *
to_bcm2712_domain(struct iommu_domain *domain)
{
	return container_of(domain, struct bcm2712_iommu_domain, base);
}

static void bcm2712_iommu_init(struct bcm2712_iommu *mmu)
{
	unsigned int bigpage_width, superpage_width;
	u32 u = MMU_RD(MMMU_DEBUG_INFO_OFFSET);
	u32 pa_width = FIELD_GET(MMMU_DEBUG_INFO_PA_WIDTH_MASK, u);

	dev_dbg(mmu->dev, "DEBUG_INFO = 0x%08x\n", u);
	WARN_ON(FIELD_GET(MMMU_DEBUG_INFO_VERSION_MASK, u) < 4 ||
		FIELD_GET(MMMU_DEBUG_INFO_VA_WIDTH_MASK, u) < 6 ||
		pa_width < 6 || !(u & MMMU_DEBUG_INFO_BYPASS));

	dma_set_mask_and_coherent(mmu->dev, DMA_BIT_MASK(pa_width + 30u));

	bigpage_width = FIELD_GET(MMMU_DEBUG_INFO_BIGPAGE_WIDTH_MASK, u);
	if (bigpage_width)
		mmu->bigpage_size = IOMMU_PAGE_SIZE << bigpage_width;

	superpage_width = FIELD_GET(MMMU_DEBUG_INFO_SUPERPAGE_WIDTH_MASK, u);
	if (superpage_width)
		mmu->superpage_size = IOMMU_PAGE_SIZE << superpage_width;

	/* Disable MMU and clear sticky flags; meanwhile flush the TLB */
	MMU_WR(MMMU_CTRL_OFFSET, MMMU_CTRL_CAP_EXCEEDED | MMMU_CTRL_PT_INVALID |
					 MMMU_CTRL_WRITE_VIOLATION |
					 MMMU_CTRL_STATS_CLEAR |
					 MMMU_CTRL_TLB_CLEAR);

	/* Put MMU into 2-level mode */
	MMU_WR(MMMU_MISC_OFFSET,
	       MMU_RD(MMMU_MISC_OFFSET) & ~MMMU_MISC_SINGLE_TABLE);
}

/*
 * Since the BCM2712 IOMMU is address-based (not device based), we don't
 * need to change any hardware state to support identity mapping.
 * The IOMMU is natively bypassed for addresses outside the aperture.
 */
static int bcm2712_iommu_identity_attach(struct iommu_domain *identity_domain,
					 struct device *dev,
					 struct iommu_domain *old)
{
	struct bcm2712_iommu *mmu = dev_iommu_priv_get(dev);
	unsigned long flags;

	spin_lock_irqsave(&mmu->hw_lock, flags);
	MMU_WR(MMMU_CTRL_OFFSET, 0);
	mmu->domain = NULL;
	spin_unlock_irqrestore(&mmu->hw_lock, flags);

	return 0;
}

static struct iommu_domain bcm2712_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev = bcm2712_iommu_identity_attach,
	},
};

static int bcm2712_iommu_blocking_attach(struct iommu_domain *blocking_domain,
					 struct device *dev,
					 struct iommu_domain *old)
{
	struct bcm2712_iommu *mmu = dev_iommu_priv_get(dev);
	unsigned long flags;

	spin_lock_irqsave(&mmu->hw_lock, flags);

	/*
	 * To completely block DMA:
	 * 1. Disable the bypass window (requests outside aperture will abort).
	 * 2. Set the address cap to 0 (requests inside aperture will abort).
	 */
	MMU_WR(MMMU_BYPASS_START_OFFSET, 0);
	MMU_WR(MMMU_BYPASS_END_OFFSET, 0);
	MMU_WR(MMMU_ADDR_CAP_OFFSET, MMMU_ADDR_CAP_ENABLE);

	MMU_WR(MMMU_CTRL_OFFSET, MMMU_CTRL_OPERATING_FLAGS);

	mmu->domain = NULL;
	spin_unlock_irqrestore(&mmu->hw_lock, flags);

	return 0;
}

static struct iommu_domain bcm2712_blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &(const struct iommu_domain_ops) {
		.attach_dev = bcm2712_iommu_blocking_attach,
	},
};

static int bcm2712_iommu_attach_dev(struct iommu_domain *domain,
				    struct device *dev,
				    struct iommu_domain *old)
{
	struct bcm2712_iommu *mmu = dev_iommu_priv_get(dev);
	struct bcm2712_iommu_domain *mydomain = to_bcm2712_domain(domain);
	struct pt_iommu_bcm2712_hw_info info;
	u32 default_page_pfn, u;
	unsigned int byp_shift;
	unsigned long flags;

	spin_lock_irqsave(&mmu->hw_lock, flags);

	if (mmu->domain == mydomain)
		goto unlock;

	mmu->domain = mydomain;

	/*
	 * This driver is for VC IOMMU version >= 4 and assumes at least 36
	 * bits of virtual and physical address space.
	 */
	u = MMU_RD(MMMU_DEBUG_INFO_OFFSET);
	byp_shift = (u & MMMU_DEBUG_INFO_BYPASS_4M) ? IOMMU_HUGEPAGE_SHIFT :
						      ADDR_CAP_SHIFT;

	/*
	 * Set address cap and bypass range (note unintuitive off-by-ones).
	 * Requests to the bypass window pass straight through unchanged: this
	 * is useful for blocks which share an IOMMU with other blocks whose
	 * drivers are not IOMMU-aware.
	 */
	MMU_WR(MMMU_ADDR_CAP_OFFSET,
	       MMMU_ADDR_CAP_ENABLE +
	       (BCM2712_APERTURE_END >> ADDR_CAP_SHIFT) - 1);
	MMU_WR(MMMU_BYPASS_START_OFFSET, 0);
	MMU_WR(MMMU_BYPASS_END_OFFSET,
	       MMMU_BYPASS_END_ENABLE + (BCM2712_APERTURE_BASE >> byp_shift));

	/*
	 * When the IOMMU handles a request, it adds the PT_PA_BASE_OFFSET to
	 * (IOVA>>32) to calculate the PFN of the corresponding L1 directory page.
	 * IOVA bits [31:22] are then used to fetch the L1 descriptor within
	 * (which in turn points to the L2 table).
	 * This clever logic would allow for a L1 table larger than 4kb (and hence
	 * a larger aperture.
	 */
	pt_iommu_bcm2712_hw_info(&mydomain->pt, &info);
	MMU_WR(MMMU_PT_PA_BASE_OFFSET, (info.pt_base >> IOMMU_PAGE_SHIFT) -
				       (BCM2712_APERTURE_BASE >> 32));

	/* Set up a default (error) page used to catch illegal reads/writes */
	default_page_pfn = virt_to_phys(mydomain->default_page) >>
			   IOMMU_PAGE_SHIFT;
	MMU_WR(MMMU_ILLEGAL_ADR_OFFSET,
	       MMMU_ILLEGAL_ADR_ENABLE + default_page_pfn);

	/* Flush (and enable) the shared TLB cache; enable this MMU. */
	bcm2712_iommu_cache_flush(mmu->cache);
	MMU_WR(MMMU_CTRL_OFFSET, MMMU_CTRL_OPERATING_FLAGS);

unlock:
	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	return 0;
}

static void bcm2712_iommu_shootdown_range(struct bcm2712_iommu *mmu,
					  unsigned long iova, size_t size)
{
	unsigned long iova_end = iova + size - 1;
	unsigned int page_group;
	u32 val;

	/* Shootdown register deals with 4 pages at a time */
	for (page_group = iova >> (IOMMU_PAGE_SHIFT + 2);
	     page_group <= iova_end >> (IOMMU_PAGE_SHIFT + 2); page_group++) {
		MMU_WR(MMMU_SHOOT_DOWN_OFFSET,
		       MMMU_SHOOT_DOWN_SHOOT + (page_group << 2));
		readl_poll_timeout_atomic(
			mmu->reg_base + MMMU_SHOOT_DOWN_OFFSET, val,
			!(val & MMMU_SHOOT_DOWN_SHOOTING), 0, 1000);
	}
}

static int bcm2712_iommu_sync_range(struct iommu_domain *domain,
				    unsigned long iova, size_t size)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&mmu->hw_lock, flags);
	bcm2712_iommu_cache_flush(mmu->cache);

	/* If invalidating more than 16MB, just do a full TLB clear */
	if (size >= SZ_16M) {
		MMU_WR(MMMU_CTRL_OFFSET,
		       MMMU_CTRL_OPERATING_FLAGS | MMMU_CTRL_TLB_CLEAR);
		readl_poll_timeout_atomic(mmu->reg_base + MMMU_CTRL_OFFSET, val,
					  !(val & MMMU_CTRL_TLB_CLEARING), 0,
					  1000);
	} else {
		bcm2712_iommu_shootdown_range(mmu, iova, size);
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	return 0;
}

static void bcm2712_iommu_sync(struct iommu_domain *domain,
			       struct iommu_iotlb_gather *gather)
{
	bcm2712_iommu_sync_range(domain, gather->start,
				 gather->end - gather->start + 1);
}

static int bcm2712_iommu_sync_map(struct iommu_domain *domain,
				  unsigned long iova, size_t size)
{
	return bcm2712_iommu_sync_range(domain, iova, size);
}

static void bcm2712_iommu_sync_all(struct iommu_domain *domain)
{
	size_t aperture_size = domain->geometry.aperture_end -
			       domain->geometry.aperture_start + 1;

	bcm2712_iommu_sync_range(domain, domain->geometry.aperture_start,
				 aperture_size);
}

static void bcm2712_iommu_domain_free(struct iommu_domain *domain)
{
	struct bcm2712_iommu_domain *mydomain = to_bcm2712_domain(domain);
	struct bcm2712_iommu *mmu = mydomain->mmu;

	if (mmu && mmu->domain == mydomain) {
		unsigned long flags;

		spin_lock_irqsave(&mmu->hw_lock, flags);
		MMU_WR(MMMU_CTRL_OFFSET, 0);
		mmu->domain = NULL;
		spin_unlock_irqrestore(&mmu->hw_lock, flags);
	}

	pt_iommu_deinit(&mydomain->pt.iommu);
	if (mydomain->default_page)
		iommu_free_pages(mydomain->default_page);
	kfree(mydomain);
}

static const struct iommu_domain_ops bcm2712_paging_domain_ops = {
	IOMMU_PT_DOMAIN_OPS(bcm2712),
	.attach_dev	 = bcm2712_iommu_attach_dev,
	.iotlb_sync      = bcm2712_iommu_sync,
	.iotlb_sync_map  = bcm2712_iommu_sync_map,
	.flush_iotlb_all = bcm2712_iommu_sync_all,
	.free		 = bcm2712_iommu_domain_free,
};

static struct iommu_domain *bcm2712_iommu_domain_alloc(struct device *dev)
{
	struct bcm2712_iommu *mmu = dev_iommu_priv_get(dev);
	struct bcm2712_iommu_domain *domain;
	struct pt_iommu_bcm2712_cfg cfg;
	int ret;

	domain = kzalloc_obj(*domain);
	if (!domain)
		return NULL;

	domain->mmu = mmu;
	domain->pt.iommu.iommu_device = mmu->dev;
	memset(&cfg, 0, sizeof(cfg));
	cfg.common.features = BIT(PT_FEAT_DMA_INCOHERENT);

	/* Bigpage and superpage sizes are typically 64K and 1M, but may vary */
	if (mmu->bigpage_size)
		cfg.bigpage_lg2 = ilog2(mmu->bigpage_size);
	if (mmu->superpage_size)
		cfg.superpage_lg2 = ilog2(mmu->superpage_size);

	/* 2-level format: 10-bit L1 + 10-bit L2 + 12-bit page offset */
	cfg.common.hw_max_vasz_lg2 =
		(2 * PTES_PER_IOPG_SHIFT) + IOMMU_PAGE_SHIFT;

	/* PTEs encode a 28-bit output address PFN */
	cfg.common.hw_max_oasz_lg2 = 28 + IOMMU_PAGE_SHIFT;

	ret = pt_iommu_bcm2712_init(&domain->pt, &cfg, GFP_KERNEL);
	if (ret)
		goto err;

	/* Set up a default (error) page used to catch illegal reads/writes */
	domain->default_page = iommu_alloc_pages_sz(GFP_KERNEL, PAGE_SIZE);
	if (!domain->default_page)
		goto err;

	domain->base.geometry.aperture_start = BCM2712_APERTURE_BASE;
	domain->base.geometry.aperture_end = BCM2712_APERTURE_END - 1;
	domain->base.geometry.force_aperture = true;
	domain->base.ops = &bcm2712_paging_domain_ops;
	return &domain->base;

err:
	bcm2712_iommu_domain_free(&domain->base);
	return NULL;
}

static struct bcm2712_iommu *
bcm2712_iommu_get_by_fwnode(struct fwnode_handle *fwnode)
{
	struct device *dev =
		bus_find_device_by_fwnode(&platform_bus_type, fwnode);
	struct bcm2712_iommu *mmu;

	if (!dev)
		return NULL;

	mmu = dev_get_drvdata(dev);
	put_device(dev);
	return mmu;
}

static struct iommu_device *bcm2712_iommu_probe_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct bcm2712_iommu *mmu;

	if (!fwspec || !fwspec->iommu_fwnode)
		return ERR_PTR(-ENODEV);

	mmu = bcm2712_iommu_get_by_fwnode(fwspec->iommu_fwnode);
	if (!mmu)
		return ERR_PTR(-ENODEV);

	dev_iommu_priv_set(dev, mmu);

	return &mmu->iommu;
}

static int bcm2712_iommu_of_xlate(struct device *dev,
				  const struct of_phandle_args *args)
{
	return iommu_fwspec_add_ids(dev, args->args, 0);
}

static const struct iommu_ops bcm2712_iommu_ops = {
	.identity_domain = &bcm2712_identity_domain,
	.blocked_domain = &bcm2712_blocking_domain,
	.domain_alloc_paging = bcm2712_iommu_domain_alloc,
	.probe_device = bcm2712_iommu_probe_device,
	.device_group = generic_single_device_group,
	.of_xlate = bcm2712_iommu_of_xlate,
};

static const struct of_device_id bcm2712_iommu_of_match[] = {
	{ .compatible = "brcm,bcm2712-iommu" },
	{ /* sentinel */ }
};

static int bcm2712_iommu_init_cache(struct bcm2712_iommu *mmu,
				    struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *cache_pdev;
	struct device_node *cache_np;

	cache_np = of_parse_phandle(dev->of_node, "brcm,iommu-cache", 0);

	/* Fall back on 'cache' property used in old/downstream firmware */
	if (!cache_np)
		cache_np = of_parse_phandle(dev->of_node, "cache", 0);

	if (!cache_np)
		return dev_err_probe(dev, -ENOENT,
				     "missing brcm,iommu-cache property\n");

	cache_pdev = of_find_device_by_node(cache_np);
	of_node_put(cache_np);
	if (!cache_pdev)
		return -EPROBE_DEFER;

	mmu->cache = platform_get_drvdata(cache_pdev);
	if (!mmu->cache) {
		put_device(&cache_pdev->dev);
		return -EPROBE_DEFER;
	}

	put_device(&cache_pdev->dev);
	return 0;
}

static int bcm2712_iommu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	struct bcm2712_iommu *mmu;
	int ret;

	mmu = devm_kzalloc(dev, sizeof(*mmu), GFP_KERNEL);
	if (!mmu)
		return -ENOMEM;

	mmu->dev = dev;
	spin_lock_init(&mmu->hw_lock);

	mmu->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmu->reg_base))
		return PTR_ERR(mmu->reg_base);

	ret = bcm2712_iommu_init_cache(mmu, pdev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mmu);
	bcm2712_iommu_init(mmu);

	ret = iommu_device_sysfs_add(&mmu->iommu, dev, NULL, dev_name(dev));
	if (ret)
		return ret;

	ret = iommu_device_register(&mmu->iommu, &bcm2712_iommu_ops, dev);
	if (ret) {
		iommu_device_sysfs_remove(&mmu->iommu);
		return ret;
	}

	return 0;
}

static struct platform_driver bcm2712_iommu_driver = {
	.driver	= {
		.name		= "bcm2712-iommu",
		.of_match_table	= bcm2712_iommu_of_match,
		.suppress_bind_attrs = true,
	},
	.probe	= bcm2712_iommu_probe,
};
builtin_platform_driver(bcm2712_iommu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel Drake <dan@reactivated.net>");
MODULE_DESCRIPTION("Broadcom BCM2712 IOMMU driver");
