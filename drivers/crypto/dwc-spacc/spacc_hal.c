// SPDX-License-Identifier: GPL-2.0

#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include "spacc_hal.h"

static struct dma_pool *ddt_pool, *ddt16_pool, *ddt4_pool;
static struct device *ddt_device;

#define PDU_REG_SPACC_VERSION   0x00180UL
#define PDU_REG_SPACC_CONFIG    0x00184UL
#define PDU_REG_SPACC_CONFIG2   0x00190UL
#define PDU_REG_SPACC_IV_OFFSET 0x00040UL
#define PDU_REG_PDU_CONFIG      0x00188UL
#define PDU_REG_SECURE_LOCK     0x001C0UL

#define DDT_MAX_ENTRIES		((PDU_MAX_DDT + 1) * 8)
#define DDT_16_ENTRIES		((16 + 1) * 8)
#define DDT_4_ENTRIES		((4 + 1) * 8)

int pdu_get_version(void __iomem *dev, struct pdu_info *inf)
{
	unsigned long reg_val;

	if (!inf)
		return -EINVAL;

	memset(inf, 0, sizeof(*inf));
	reg_val = readl(dev + PDU_REG_SPACC_VERSION);

	/*
	 * Read the SPAcc version block this tells us the revision,
	 * project, and a few other feature bits
	 *
	 * layout for v6.5+
	 */
	inf->spacc_version = (struct spacc_version_block) {
		.minor      = SPACC_ID_MINOR(reg_val),
		.major      = SPACC_ID_MAJOR(reg_val),
		.version    = (SPACC_ID_MAJOR(reg_val) << 4) |
			       SPACC_ID_MINOR(reg_val),
		.qos        = SPACC_ID_QOS(reg_val),
		.is_spacc   = SPACC_ID_TYPE(reg_val) == SPACC_TYPE_SPACCQOS,
		.is_pdu     = SPACC_ID_TYPE(reg_val) == SPACC_TYPE_PDU,
		.aux        = SPACC_ID_AUX(reg_val),
		.vspacc_id  = SPACC_ID_VIDX(reg_val),
		.partial    = SPACC_ID_PARTIAL(reg_val),
		.project    = SPACC_ID_PROJECT(reg_val),
	};

	/* try to autodetect */
	writel(0x80000000, dev + PDU_REG_SPACC_IV_OFFSET);

	if (readl(dev + PDU_REG_SPACC_IV_OFFSET) == 0x80000000)
		inf->spacc_version.ivimport = 1;
	else
		inf->spacc_version.ivimport = 0;

	/*
	 * Read the SPAcc config block (v6.5+) which tells us how many
	 * contexts there are and context page sizes
	 * this register is only available in v6.5 and up
	 */
	reg_val = readl(dev + PDU_REG_SPACC_CONFIG);
	inf->spacc_config = (struct spacc_config_block) {
		SPACC_CFG_CTX_CNT(reg_val),
		SPACC_CFG_VSPACC_CNT(reg_val),
		SPACC_CFG_CIPH_CTX_SZ(reg_val),
		SPACC_CFG_HASH_CTX_SZ(reg_val),
		SPACC_CFG_DMA_TYPE(reg_val),
		0, 0, 0, 0
	};

	/* CONFIG2 only present in v6.5+ cores */
	reg_val = readl(dev + PDU_REG_SPACC_CONFIG2);
	if (inf->spacc_version.qos) {
		inf->spacc_config.cmd0_fifo_depth =
				SPACC_CFG_CMD0_FIFO_QOS(reg_val);
		inf->spacc_config.cmd1_fifo_depth =
				SPACC_CFG_CMD1_FIFO(reg_val);
		inf->spacc_config.cmd2_fifo_depth =
				SPACC_CFG_CMD2_FIFO(reg_val);
		inf->spacc_config.stat_fifo_depth =
				SPACC_CFG_STAT_FIFO_QOS(reg_val);
	} else {
		inf->spacc_config.cmd0_fifo_depth =
				SPACC_CFG_CMD0_FIFO(reg_val);
		inf->spacc_config.stat_fifo_depth =
				SPACC_CFG_STAT_FIFO(reg_val);
	}

	/* only read PDU config if it's actually a PDU engine */
	if (inf->spacc_version.is_pdu) {
		reg_val = readl(dev + PDU_REG_PDU_CONFIG);
		inf->pdu_config = (struct pdu_config_block)
			{SPACC_PDU_CFG_MINOR(reg_val),
			 SPACC_PDU_CFG_MAJOR(reg_val)};

		/* unlock all cores by default */
		writel(0, dev + PDU_REG_SECURE_LOCK);
	}

	return 0;
}

void pdu_to_dev(void __iomem *addr_, uint32_t *src, unsigned long nword)
{
	void __iomem *addr = addr_;

	while (nword--) {
		writel(*src++, addr);
		addr += 4;
	}
}

void pdu_from_dev(u32 *dst, void __iomem *addr_, unsigned long nword)
{
	void __iomem *addr = addr_;

	while (nword--) {
		*dst++ = readl(addr);
		addr += 4;
	}
}

static void pdu_to_dev_big(void __iomem *addr_, const unsigned char *src,
			   unsigned long nword)
{
	u32 __iomem *addr = addr_;
	u32 data;
	__be32 val;

	while (nword--) {
		data = *((u32 *)src);
		val = __cpu_to_be32(data);

		__raw_writel((u32 __force)val, addr);
		src += 4;
		addr++;
	}
}

static void pdu_from_dev_big(unsigned char *dst, void __iomem *addr_,
			     unsigned long nword)
{
	u32 __iomem *addr = addr_;

	while (nword--) {
		*(u32 *)dst = __be32_to_cpu((__be32 __force)__raw_readl(addr));
		addr++;
		dst += 4;
	}
}

static void pdu_to_dev_little(void __iomem *addr_, const unsigned char *src,
			      unsigned long nword)
{
	u32 __iomem *addr = addr_;
	u32 data;
	__le32 val;

	while (nword--) {
		data = *((u32 *)src);
		val = __cpu_to_le32(data);

		__raw_writel((u32 __force)val, addr);
		src += 4;
		addr++;
	}
}

static void pdu_from_dev_little(unsigned char *dst, void __iomem *addr_,
				unsigned long nword)
{
	u32 __iomem *addr = addr_;

	while (nword--) {
		*(u32 *)dst = __le32_to_cpu((__le32 __force)__raw_readl(addr));
		addr++;
		dst += 4;
	}
}

void pdu_to_dev_s(void __iomem *addr, const unsigned char *src,
		  unsigned long nword, int big_endian)
{
	if (big_endian)
		pdu_to_dev_big(addr, src, nword);
	else
		pdu_to_dev_little(addr, src, nword);
}

void pdu_from_dev_s(unsigned char *dst, void __iomem *addr,
		    unsigned long nword, int big_endian)
{
	if (big_endian)
		pdu_from_dev_big(dst, addr, nword);
	else
		pdu_from_dev_little(dst, addr, nword);
}

void pdu_io_cached_write(struct device *dev, void __iomem *addr,
			 unsigned long val, uint32_t *cache)
{
	if (*cache == val) {
#ifdef CONFIG_CRYPTO_DEV_SPACC_DEBUG_TRACE_IO

		dev_dbg(dev, "pdu: write %.8lx -> %p (cached)\n", val, addr);
#endif
		return;
	}

	*cache = val;
	writel(val, addr);
}

struct device *get_ddt_device(void)
{
	return ddt_device;
}

/* platform specific DDT routines */

/*
 * Create a DMA pool for DDT entries this should help from splitting
 * pages for DDTs which by default are 520 bytes long meaning we would
 * otherwise waste 3576 bytes per DDT allocated...
 * we also maintain a smaller table of 4 entries common for simple jobs
 * which uses 480 fewer bytes of DMA memory.
 * and for good measure another table for 16 entries saving 384 bytes
 */
int pdu_mem_init(void *device)
{
	if (ddt_device)
		return 0; /* already setup */

	/* max of 64 DDT entries */
	ddt_device = device;
	ddt_pool = dma_pool_create("spaccddt", device,
				   DDT_MAX_ENTRIES, 8, 0);

	if (!ddt_pool)
		return -ENOSPC;

#if PDU_MAX_DDT > 16
	/* max of 16 DDT entries */
	ddt16_pool = dma_pool_create("spaccddt16", device,
				     DDT_16_ENTRIES, 8, 0);
	if (!ddt16_pool) {
		dma_pool_destroy(ddt_pool);
		return -ENOSPC;
	}
#else
	ddt16_pool = ddt_pool;
#endif
	/* max of 4 DDT entries */
	ddt4_pool = dma_pool_create("spaccddt4", device,
				    DDT_4_ENTRIES, 8, 0);
	if (!ddt4_pool) {
		dma_pool_destroy(ddt_pool);
#if PDU_MAX_DDT > 16
		dma_pool_destroy(ddt16_pool);
#endif
		return -ENOSPC;
	}

	return 0;
}

/* Destroy the pool */
void pdu_mem_deinit(void *device)
{
	/* for now, just skip deinit except for matching device */
	if (device != ddt_device)
		return;

	dma_pool_destroy(ddt_pool);

#if PDU_MAX_DDT > 16
	dma_pool_destroy(ddt16_pool);
#endif
	dma_pool_destroy(ddt4_pool);

	ddt_device = NULL;
}

int pdu_ddt_init(struct device *dev, struct pdu_ddt *ddt, unsigned long limit)
{
	/*
	 * Set the MSB if we want to use an ATOMIC
	 * allocation required for top half processing
	 */
	int flag = (limit & 0x80000000);

	limit &= 0x7FFFFFFF;
	if (limit + 1 >= SIZE_MAX / 8) {
		/* too big to even compute DDT size */
		return -EINVAL;
	} else if (limit > PDU_MAX_DDT) {
		size_t len = 8 * ((size_t)limit + 1);

		ddt->virt = dma_alloc_coherent(ddt_device, len, &ddt->phys,
					       flag ? GFP_ATOMIC : GFP_KERNEL);
	} else if (limit > 16) {
		ddt->virt = dma_pool_alloc(ddt_pool, flag ? GFP_ATOMIC :
				GFP_KERNEL, &ddt->phys);
	} else if (limit > 4) {
		ddt->virt = dma_pool_alloc(ddt16_pool, flag ? GFP_ATOMIC :
				GFP_KERNEL, &ddt->phys);
	} else {
		ddt->virt = dma_pool_alloc(ddt4_pool, flag ? GFP_ATOMIC :
				GFP_KERNEL, &ddt->phys);
	}

	ddt->idx = 0;
	ddt->len = 0;
	ddt->limit = limit;

	if (!ddt->virt)
		return -EINVAL;

#ifdef CONFIG_CRYPTO_DEV_SPACC_DEBUG_TRACE_DDT

	dev_dbg(dev, "   DDT[%.8lx]: allocated %lu fragments\n",
		(unsigned long)ddt->phys, limit);
#endif
	return 0;
}

int pdu_ddt_add(struct device *dev, struct pdu_ddt *ddt, dma_addr_t phys,
		unsigned long size)
{
#ifdef CONFIG_CRYPTO_DEV_SPACC_DEBUG_TRACE_DDT

	dev_dbg(dev, "   DDT[%.8lx]: 0x%.8lx size %lu\n",
		(unsigned long)ddt->phys,
		(unsigned long)phys, size);
#endif

	if (ddt->idx == ddt->limit)
		return -EINVAL;

	ddt->virt[ddt->idx * 2 + 0] = (uint32_t)phys;
	ddt->virt[ddt->idx * 2 + 1] = size;
	ddt->virt[ddt->idx * 2 + 2] = 0;
	ddt->virt[ddt->idx * 2 + 3] = 0;

	ddt->len += size;
	++(ddt->idx);

	return 0;
}

int pdu_ddt_free(struct pdu_ddt *ddt)
{
	if (ddt->virt) {
		if (ddt->limit > PDU_MAX_DDT) {
			size_t len = 8 * ((size_t)ddt->limit + 1);

			dma_free_coherent(ddt_device, len, ddt->virt,
					  ddt->phys);
		} else if (ddt->limit > 16) {
			dma_pool_free(ddt_pool, ddt->virt, ddt->phys);
		} else if (ddt->limit > 4) {
			dma_pool_free(ddt16_pool, ddt->virt, ddt->phys);
		} else {
			dma_pool_free(ddt4_pool, ddt->virt, ddt->phys);
		}

		ddt->virt = NULL;
	}

	return 0;
}
