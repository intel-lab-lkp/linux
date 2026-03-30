// SPDX-License-Identifier: GPL-2.0
/*
 * non-coherent cache operations for Andes Platform CPUs.
 *
 * Copyright (C) 2023 Renesas Electronics Corp.
 */

#include <linux/cacheflush.h>
#include <linux/cacheinfo.h>
#include <linux/dma-direction.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/soc/andes/csr.h>

#include <asm/dma-noncoherent.h>

/* L1 D-cache operation encoding */
#define ANDES_L1D_CCTL_VA_INVAL			0x0	/* Invalidate an L1D cacheline */
#define ANDES_L1D_CCTL_VA_WB			0x1	/* Write-back an L1D cacheline */
#define ANDES_L1D_CCTL_VA_WBINVAL		0x2	/* Flush an L1D cacheline */
#define ANDES_L1D_CCTL_WBINVAL_ALL		0x6	/* Flush the entire L1D cache */

/* LLC registers */
#define ANDES_LLC_REG_CFG_OFFSET		0x0
#define ANDES_LLC_REG_CTRL_OFFSET		0x8
#define ANDES_LLC_REG_ASYNC_ERR_OFFSET		0x30
#define ANDES_LLC_REG_ERR_OFFSET		0x38
#define ANDES_LLC_REG_CCTL_CMD_OFFSET_C0	0x40
#define ANDES_LLC_REG_CCTL_ACC_OFFSET_C0	0x48
#define ANDES_LLC_REG_CCTL_STATUS_OFFSET_C0	0x80

/* LLC CCTL status encoding */
#define ANDES_LLC_CCTL_STATUS_IDLE		0x0
#define ANDES_LLC_CCTL_STATUS_RUNNING		0x1
#define ANDES_LLC_CCTL_STATUS_ILLEGAL		0x2

/* LLC CCTL status core 0 mask */
#define ANDES_LLC_CCTL_STATUS_MASK_C0		GENMASK(3, 0)

/* LLC operation encoding */
#define ANDES_LLC_CCTL_PA_INVAL			0x8	/* Invalidate an LLC cacheline */
#define ANDES_LLC_CCTL_PA_WB			0x9	/* Write-back an LLC cacheline */
#define ANDES_LLC_CCTL_PA_WBINVAL		0xa	/* Flush an LLC cacheline */
#define ANDES_LLC_CCTL_WBINVAL_ALL		0x12	/* Flush the entire LLC cache */

/* LLC CCTL registers and fields by core */
#define ANDES_LLC_REG_PER_CORE_OFFSET		0x10
#define ANDES_CCTL_LLC_STATUS_PER_CORE_OFFSET	0x4

#define ANDES_LLC_REG_CCTL_CMD_OFFSET_BY_CORE(n)	\
	(ANDES_LLC_REG_CCTL_CMD_OFFSET_C0 + ((n) * ANDES_LLC_REG_PER_CORE_OFFSET))
#define ANDES_LLC_REG_CCTL_ACC_OFFSET_BY_CORE(n)	\
	(ANDES_LLC_REG_CCTL_ACC_OFFSET_C0 + ((n) * ANDES_LLC_REG_PER_CORE_OFFSET))
#define ANDES_LLC_CCTL_STATUS_MASK_BY_CORE(n)	\
	(ANDES_LLC_CCTL_STATUS_MASK_C0 << ((n) * ANDES_CCTL_LLC_STATUS_PER_CORE_OFFSET))

#define ANDES_CACHE_LINE_SIZE			64

struct andes_priv {
	void __iomem *llc_base;
	u32 andes_cache_line_size;
};

static struct andes_priv andes_priv;

/* LLC operations */
static inline uint32_t andes_cpu_llc_get_cctl_status(void)
{
	return readl_relaxed(andes_priv.llc_base + ANDES_LLC_REG_CCTL_STATUS_OFFSET_C0);
}

static void andes_cpu_cache_operation(unsigned long start, unsigned long end,
				       unsigned int l1_op, unsigned int llc_op)
{
	unsigned long line_size = andes_priv.andes_cache_line_size;
	void __iomem *base = andes_priv.llc_base;
	unsigned long pa;
	int mhartid = 0;

	if (IS_ENABLED(CONFIG_SMP))
		mhartid = cpuid_to_hartid_map(get_cpu());
	else
		mhartid = cpuid_to_hartid_map(0);

	mb(); /* complete earlier memory accesses before the cache flush */
	while (end > start) {
		csr_write(CSR_UCCTLBEGINADDR, start);
		csr_write(CSR_UCCTLCOMMAND, l1_op);

		pa = virt_to_phys((void *)start);
		writel_relaxed(pa, base + ANDES_LLC_REG_CCTL_ACC_OFFSET_BY_CORE(mhartid));
		writel_relaxed(llc_op, base + ANDES_LLC_REG_CCTL_CMD_OFFSET_BY_CORE(mhartid));
		while ((andes_cpu_llc_get_cctl_status() &
			ANDES_LLC_CCTL_STATUS_MASK_BY_CORE(mhartid)) !=
			ANDES_LLC_CCTL_STATUS_IDLE)
			;

		start += line_size;
	}
	mb(); /* issue later memory accesses after the cache flush */

	if (IS_ENABLED(CONFIG_SMP))
		put_cpu();
}

/* Write-back L1 and LLC entry */
static inline void andes_cpu_dcache_wb_range(unsigned long start, unsigned long end)
{
	andes_cpu_cache_operation(start, end, ANDES_L1D_CCTL_VA_WB,
				   ANDES_LLC_CCTL_PA_WB);
}

/* Invalidate the L1 and LLC entry */
static inline void andes_cpu_dcache_inval_range(unsigned long start, unsigned long end)
{
	andes_cpu_cache_operation(start, end, ANDES_L1D_CCTL_VA_INVAL,
				   ANDES_LLC_CCTL_PA_INVAL);
}

static void andes_dma_cache_inv(phys_addr_t paddr, size_t size)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);
	unsigned long end = start + size;
	unsigned long line_size = andes_priv.andes_cache_line_size;
	unsigned long flags;

	if (unlikely(!size))
		return;

	start = ALIGN_DOWN(start, line_size);
	end = ALIGN(end, line_size);

	local_irq_save(flags);
	andes_cpu_dcache_inval_range(start, end);
	local_irq_restore(flags);
}

static void andes_dma_cache_wback(phys_addr_t paddr, size_t size)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);
	unsigned long end = start + size;
	unsigned long line_size = andes_priv.andes_cache_line_size;
	unsigned long flags;

	if (unlikely(!size))
		return;

	start = ALIGN_DOWN(start, line_size);
	end = ALIGN(end, line_size);

	local_irq_save(flags);
	andes_cpu_dcache_wb_range(start, end);
	local_irq_restore(flags);
}

static void andes_dma_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	andes_dma_cache_wback(paddr, size);
	andes_dma_cache_inv(paddr, size);
}

static int andes_get_llc_line_size(struct device_node *np)
{
	int ret;

	ret = of_property_read_u32(np, "cache-line-size", &andes_priv.andes_cache_line_size);
	if (ret) {
		pr_err("Cache: Failed to get cache-line-size\n");
		return ret;
	}

	if (andes_priv.andes_cache_line_size != ANDES_CACHE_LINE_SIZE) {
		pr_warn("Cache: Expected cache-line-size to be 64 bytes (found:%u)\n",
			andes_priv.andes_cache_line_size);
	}

	return 0;
}

static const struct riscv_nonstd_cache_ops andes_cmo_ops __initconst = {
	.wback = &andes_dma_cache_wback,
	.inv = &andes_dma_cache_inv,
	.wback_inv = &andes_dma_cache_wback_inv,
};

static const struct of_device_id andes_cache_ids[] = {
	{ .compatible = "andestech,llcache" },
	{ /* sentinel */ }
};

static int __init andes_cache_init(void)
{
	struct resource res;
	int ret = 0;

	struct device_node *np __free(device_node) =
		of_find_matching_node(NULL, andes_cache_ids);
	if (!of_device_is_available(np)) {
		ret = -ENODEV;
		goto err_ret;
	}

	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		goto err_ret;

	/*
	 * If IOCP is present on the Andes AX45MP core riscv_cbom_block_size
	 * will be 0 for sure, so we can definitely rely on it. If
	 * riscv_cbom_block_size = 0 we don't need to handle CMO using SW any
	 * more so we just return success here and only if its being set we
	 * continue further in the probe path.
	 */
	if (!riscv_cbom_block_size)
		return 0;

	andes_priv.llc_base = ioremap(res.start, resource_size(&res));
	if (!andes_priv.llc_base) {
		ret = -ENOMEM;
		goto err_ret;
	}

	ret = andes_get_llc_line_size(np);
	if (ret)
		goto err_unmap;

	riscv_noncoherent_register_cache_ops(&andes_cmo_ops);

	return 0;

err_unmap:
	iounmap(andes_priv.llc_base);
err_ret:
	return ret;
}
early_initcall(andes_cache_init);
