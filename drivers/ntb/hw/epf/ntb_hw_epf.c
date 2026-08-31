// SPDX-License-Identifier: GPL-2.0
/*
 * Host side endpoint driver to implement Non-Transparent Bridge functionality
 *
 * Copyright (C) 2020 Texas Instruments
 * Author: Kishon Vijay Abraham I <kishon@ti.com>
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/dma/edma.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/ntb.h>

#define NTB_EPF_COMMAND		0x0
#define CMD_CONFIGURE_DOORBELL	1
#define CMD_TEARDOWN_DOORBELL	2
#define CMD_CONFIGURE_MW	3
#define CMD_TEARDOWN_MW		4
#define CMD_LINK_UP		5
#define CMD_LINK_DOWN		6
#define CMD_CONFIGURE_DMA	7
#define CMD_TEARDOWN_DMA	8

#define NTB_EPF_ARGUMENT	0x4
#define MSIX_ENABLE		BIT(16)

#define NTB_EPF_CMD_STATUS	0x8
#define COMMAND_STATUS_OK	1
#define COMMAND_STATUS_ERROR	2

#define NTB_EPF_LINK_STATUS	0x0A
#define LINK_STATUS_UP		BIT(0)

#define NTB_EPF_TOPOLOGY	0x0C
#define NTB_EPF_LOWER_ADDR	0x10
#define NTB_EPF_UPPER_ADDR	0x14
#define NTB_EPF_LOWER_SIZE	0x18
#define NTB_EPF_UPPER_SIZE	0x1C
#define NTB_EPF_MW_COUNT	0x20
#define NTB_EPF_MW1_OFFSET	0x24
#define NTB_EPF_SPAD_OFFSET	0x28
#define NTB_EPF_SPAD_COUNT	0x2C
#define NTB_EPF_DB_ENTRY_SIZE	0x30
#define NTB_EPF_DB_DATA(n)	(0x34 + (n) * 4)
#define NTB_EPF_DB_OFFSET(n)	(0xB4 + (n) * 4)

/* Private DMA wire extension produced by pci-epf-vntb. */
#define NTB_EPF_DMA_BASE	0x134
#define NTB_EPF_DMA_MAGIC	(NTB_EPF_DMA_BASE + 0x00)
#define NTB_EPF_DMA_REV_LEN	(NTB_EPF_DMA_BASE + 0x04)
#define NTB_EPF_DMA_TYPE	(NTB_EPF_DMA_BASE + 0x08)
#define NTB_EPF_DMA_REGION_BAR(base)	((base) + 0x00)
#define NTB_EPF_DMA_REGION_OFFSET(base)	((base) + 0x04)
#define NTB_EPF_DMA_REGION_SIZE(base)	((base) + 0x08)
#define NTB_EPF_DMA_REGION_SIZEOF	0x0C
#define NTB_EPF_DMA_SUBMAP_BASE	(NTB_EPF_DMA_BASE + 0x0C)
#define NTB_EPF_DMA_COMMON_SIZE	0x18
#define NTB_EPF_DMA_REG_BASE	(NTB_EPF_DMA_BASE + NTB_EPF_DMA_COMMON_SIZE)
#define NTB_EPF_DMA_CHAN_BASE	(NTB_EPF_DMA_REG_BASE + \
					 NTB_EPF_DMA_REGION_SIZEOF)
#define NTB_EPF_DMA_CHAN_SIZE	0x14
#define NTB_EPF_DMA_CHAN_DESC_BASE(n)	(NTB_EPF_DMA_CHAN_BASE + \
					 (n) * NTB_EPF_DMA_CHAN_SIZE + 0x00)
#define NTB_EPF_DMA_CHAN_DESC_ADDR_LO(n)	(NTB_EPF_DMA_CHAN_BASE + \
					 (n) * NTB_EPF_DMA_CHAN_SIZE + 0x0C)
#define NTB_EPF_DMA_CHAN_DESC_ADDR_HI(n)	(NTB_EPF_DMA_CHAN_BASE + \
					 (n) * NTB_EPF_DMA_CHAN_SIZE + 0x10)

#define NTB_EPF_DMA_MAGIC_VALUE	0x414d444e /* "NDMA": NTB DMA */
#define NTB_EPF_DMA_REVISION	1
#define NTB_EPF_DMA_TYPE_DW_EDMA	1

/*
 * Legacy doorbell slot layout when paired with pci-epf-*ntb:
 *
 *   slot 0 : reserved for link events
 *   slot 1 : unused (historical extra offset)
 *   slot 2 : DB#0
 *   slot 3 : DB#1
 *   ...
 *
 * Thus, NTB_EPF_MIN_DB_COUNT=3 means that we at least create vectors for
 * doorbells DB#0 and DB#1.
 */
#define NTB_EPF_MIN_DB_COUNT	3
#define NTB_EPF_MAX_DB_COUNT	31

#define NTB_EPF_COMMAND_TIMEOUT	1000 /* 1 Sec */

enum pci_barno {
	NO_BAR = -1,
	BAR_0,
	BAR_1,
	BAR_2,
	BAR_3,
	BAR_4,
	BAR_5,
};

enum epf_ntb_bar {
	BAR_CONFIG,
	BAR_PEER_SPAD,
	BAR_DB,
	BAR_MW1,
	BAR_MW2,
	BAR_MW3,
	BAR_MW4,
	NTB_BAR_NUM,
};

enum epf_irq_slot {
	EPF_IRQ_LINK = 0,
	EPF_IRQ_RESERVED_DB, /* Historically skipped slot */
	EPF_IRQ_DB_START,
};

#define NTB_EPF_MAX_MW_COUNT	(NTB_BAR_NUM - BAR_MW1)

struct ntb_epf_dev;

struct ntb_epf_irq_ctx {
	struct ntb_epf_dev *ndev;
	unsigned int irq_no;
};

struct ntb_epf_dma_region {
	unsigned int bar;
	resource_size_t offset;
	resource_size_t size;
	void __iomem *vaddr;
};

struct ntb_epf_dw_edma {
	struct dw_edma_chip chip;
	struct ntb_epf_dma_region reg;
	struct ntb_epf_dma_region ll[EDMA_MAX_RD_CH];
};

struct ntb_epf_dma {
	struct ntb_epf_dma_region submap;
	unsigned int nr_irqs;
	u32 type;
};

struct ntb_epf_dev {
	struct ntb_dev ntb;
	struct device *dev;
	/* Mutex to protect providing commands to NTB EPF */
	struct mutex cmd_lock;

	const enum pci_barno *barno_map;

	unsigned int mw_count;
	unsigned int spad_count;
	unsigned int db_count;
	struct ntb_epf_dma dma;
	struct ntb_epf_dw_edma dw_edma;

	void __iomem *ctrl_reg;
	void __iomem *db_reg;
	void __iomem *peer_spad_reg;

	unsigned int self_spad;
	unsigned int peer_spad;

	atomic64_t db_val;
	u64 db_valid_mask;
	struct ntb_epf_irq_ctx irq_ctx[NTB_EPF_MAX_DB_COUNT + 1];
};

#define ntb_ndev(__ntb) container_of(__ntb, struct ntb_epf_dev, ntb)

static int ntb_epf_send_command(struct ntb_epf_dev *ndev, u32 command,
				u32 argument)
{
	ktime_t timeout;
	bool timedout;
	int ret = 0;
	u32 status;

	mutex_lock(&ndev->cmd_lock);
	writel(argument, ndev->ctrl_reg + NTB_EPF_ARGUMENT);
	writel(command, ndev->ctrl_reg + NTB_EPF_COMMAND);

	timeout = ktime_add_ms(ktime_get(), NTB_EPF_COMMAND_TIMEOUT);
	while (1) {
		timedout = ktime_after(ktime_get(), timeout);
		status = readw(ndev->ctrl_reg + NTB_EPF_CMD_STATUS);

		if (status == COMMAND_STATUS_ERROR) {
			ret = -EINVAL;
			break;
		}

		if (status == COMMAND_STATUS_OK)
			break;

		if (WARN_ON(timedout)) {
			ret = -ETIMEDOUT;
			break;
		}

		usleep_range(5, 10);
	}

	writew(0, ndev->ctrl_reg + NTB_EPF_CMD_STATUS);
	mutex_unlock(&ndev->cmd_lock);

	return ret;
}

static bool ntb_epf_dma_region_parse(struct ntb_epf_dev *ndev, u32 base,
				     bool optional,
				     struct ntb_epf_dma_region *region)
{
	resource_size_t bar_len;
	u32 bar, offset, size;

	bar = readl(ndev->ctrl_reg + NTB_EPF_DMA_REGION_BAR(base));
	offset = readl(ndev->ctrl_reg + NTB_EPF_DMA_REGION_OFFSET(base));
	size = readl(ndev->ctrl_reg + NTB_EPF_DMA_REGION_SIZE(base));
	region->bar = bar;
	region->offset = offset;
	region->size = size;
	if (!size)
		return optional && bar == U32_MAX && !offset;
	if (bar > BAR_5)
		return false;

	bar_len = pci_resource_len(ndev->ntb.pdev, bar);
	if (offset > bar_len || size > bar_len - offset)
		return false;

	return true;
}

/* DW eDMA */

static int ntb_epf_dw_edma_parse(struct ntb_epf_dev *ndev, u32 length)
{
	struct ntb_epf_dw_edma *edma = &ndev->dw_edma;
	struct dw_edma_chip *chip = &edma->chip;
	u32 count, chan_offset;
	unsigned int i;

	chan_offset = NTB_EPF_DMA_CHAN_BASE - NTB_EPF_DMA_BASE;
	if (length < chan_offset + NTB_EPF_DMA_CHAN_SIZE ||
	    (length - chan_offset) % NTB_EPF_DMA_CHAN_SIZE)
		return -EINVAL;

	count = (length - chan_offset) / NTB_EPF_DMA_CHAN_SIZE;
	if (count > EDMA_MAX_RD_CH)
		return -EINVAL;

	if (!ntb_epf_dma_region_parse(ndev, NTB_EPF_DMA_REG_BASE, false,
				      &edma->reg) ||
	    !IS_ALIGNED(edma->reg.offset, sizeof(u32)))
		return -EINVAL;

	chip->mf = EDMA_MF_EDMA_UNROLL;
	chip->nr_irqs = count;
	chip->ll_rd_cnt = count;
	chip->func_no = PCI_FUNC(ndev->ntb.pdev->devfn);

	for (i = 0; i < count; i++) {
		u64 paddr;

		if (!ntb_epf_dma_region_parse(ndev,
					      NTB_EPF_DMA_CHAN_DESC_BASE(i),
					      false, &edma->ll[i]) ||
		    !IS_ALIGNED(edma->ll[i].offset, sizeof(u32)))
			return -EINVAL;

		paddr = readl(ndev->ctrl_reg +
			      NTB_EPF_DMA_CHAN_DESC_ADDR_LO(i));
		paddr |= (u64)readl(ndev->ctrl_reg +
				    NTB_EPF_DMA_CHAN_DESC_ADDR_HI(i)) << 32;
		if (paddr == U64_MAX)
			return -EINVAL;

		chip->ll_region_rd[i].sz = edma->ll[i].size;
		chip->ll_region_rd[i].paddr = paddr;
	}
	ndev->dma.nr_irqs = count;

	return 0;
}

static int ntb_epf_dw_edma_irq_vector(struct device *dev, unsigned int nr)
{
	struct ntb_epf_dev *ndev = dev_get_drvdata(dev);

	/* DMA vectors follow the NTB link and doorbell vector block. */
	return pci_irq_vector(ndev->ntb.pdev, ndev->db_count + 1 + nr);
}

static const struct dw_edma_plat_ops ntb_epf_dw_edma_ops = {
	.irq_vector = ntb_epf_dw_edma_irq_vector,
};

static int ntb_epf_dw_edma_map_region(struct pci_dev *pdev,
				      struct ntb_epf_dma_region *region)
{
	region->vaddr = pci_iomap_range(pdev, region->bar, region->offset,
					region->size);
	return region->vaddr ? 0 : -ENOMEM;
}

static void ntb_epf_dw_edma_unmap_regions(struct ntb_epf_dev *ndev)
{
	struct ntb_epf_dw_edma *edma = &ndev->dw_edma;
	struct pci_dev *pdev = ndev->ntb.pdev;
	unsigned int i;

	for (i = 0; i < edma->chip.ll_rd_cnt; i++) {
		if (!edma->ll[i].vaddr)
			continue;
		pci_iounmap(pdev, edma->ll[i].vaddr);
	}
	if (edma->reg.vaddr)
		pci_iounmap(pdev, edma->reg.vaddr);
}

static int ntb_epf_dw_edma_init(struct ntb_epf_dev *ndev)
{
	struct ntb_epf_dw_edma *edma = &ndev->dw_edma;
	struct dw_edma_chip *chip = &edma->chip;
	struct pci_dev *pdev = ndev->ntb.pdev;
	unsigned int i;
	int ret;

	/* Install the endpoint BAR submaps before mapping the advertised regions. */
	ret = ntb_epf_send_command(ndev, CMD_CONFIGURE_DMA, 0);
	if (ret)
		return ret;

	ret = ntb_epf_dw_edma_map_region(pdev, &edma->reg);
	if (ret)
		goto err_teardown;
	for (i = 0; i < chip->ll_rd_cnt; i++) {
		ret = ntb_epf_dw_edma_map_region(pdev, &edma->ll[i]);
		if (ret)
			goto err_iounmap;
		chip->ll_region_rd[i].vaddr.io = edma->ll[i].vaddr;
	}

	chip->dev = ndev->dev;
	chip->ops = &ntb_epf_dw_edma_ops;
	chip->flags = DW_EDMA_CHIP_PARTIAL;
	chip->reg_base = edma->reg.vaddr;

	ret = dw_edma_probe(chip);
	if (ret)
		goto err_iounmap;

	return 0;

err_iounmap:
	ntb_epf_dw_edma_unmap_regions(ndev);
err_teardown:
	ntb_epf_send_command(ndev, CMD_TEARDOWN_DMA, 0);
	return ret;
}

static void ntb_epf_dw_edma_deinit(struct ntb_epf_dev *ndev)
{
	struct ntb_epf_dw_edma *edma = &ndev->dw_edma;
	int ret;

	dw_edma_remove(&edma->chip);
	ntb_epf_dw_edma_unmap_regions(ndev);

	ret = ntb_epf_send_command(ndev, CMD_TEARDOWN_DMA, 0);
	if (ret)
		dev_warn(ndev->dev, "Failed to teardown endpoint DMA\n");
}

/* Common endpoint DMA */

static int ntb_epf_dma_parse(struct ntb_epf_dev *ndev)
{
	u32 magic, rev_len, length, type;
	u32 spad_off;
	int ret;

	/*
	 * Legacy endpoints place scratchpads where this extension would begin.
	 * Once the extension is present, malformed metadata is fatal.
	 */
	spad_off = readl(ndev->ctrl_reg + NTB_EPF_SPAD_OFFSET);
	if (spad_off < NTB_EPF_DMA_BASE + NTB_EPF_DMA_COMMON_SIZE)
		return 0;

	magic = readl(ndev->ctrl_reg + NTB_EPF_DMA_MAGIC);
	if (!magic)
		return 0;
	if (magic == U32_MAX)
		return -EIO;
	if (magic != NTB_EPF_DMA_MAGIC_VALUE)
		return -EINVAL;

	rev_len = readl(ndev->ctrl_reg + NTB_EPF_DMA_REV_LEN);
	length = upper_16_bits(rev_len);
	if (lower_16_bits(rev_len) != NTB_EPF_DMA_REVISION ||
	    length < NTB_EPF_DMA_COMMON_SIZE ||
	    spad_off < NTB_EPF_DMA_BASE + length)
		return -EINVAL;
	if (!ntb_epf_dma_region_parse(ndev, NTB_EPF_DMA_SUBMAP_BASE, true,
				      &ndev->dma.submap))
		return -EINVAL;

	type = readl(ndev->ctrl_reg + NTB_EPF_DMA_TYPE);
	if (type == U32_MAX)
		return -EIO;

	switch (type) {
	case NTB_EPF_DMA_TYPE_DW_EDMA:
		ret = ntb_epf_dw_edma_parse(ndev, length);
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (!ret)
		ndev->dma.type = type;

	return ret;
}

static int ntb_epf_dma_init(struct ntb_epf_dev *ndev)
{
	switch (ndev->dma.type) {
	case 0:
		return 0;
	case NTB_EPF_DMA_TYPE_DW_EDMA:
		return ntb_epf_dw_edma_init(ndev);
	default:
		return -EOPNOTSUPP;
	}
}

static void ntb_epf_dma_deinit(struct ntb_epf_dev *ndev)
{
	switch (ndev->dma.type) {
	case NTB_EPF_DMA_TYPE_DW_EDMA:
		ntb_epf_dw_edma_deinit(ndev);
		break;
	}
}

static int ntb_epf_mw_to_bar(struct ntb_epf_dev *ndev, int idx)
{
	struct device *dev = ndev->dev;

	if (idx < 0 || idx > ndev->mw_count) {
		dev_err(dev, "Unsupported Memory Window index %d\n", idx);
		return -EINVAL;
	}

	return ndev->barno_map[BAR_MW1 + idx];
}

static resource_size_t ntb_epf_mw_offset(struct ntb_epf_dev *ndev, int idx)
{
	return !idx ? readl(ndev->ctrl_reg + NTB_EPF_MW1_OFFSET) : 0;
}

static resource_size_t ntb_epf_mw_size(struct ntb_epf_dev *ndev, int idx,
				       int bar)
{
	resource_size_t offset, end;

	offset = ntb_epf_mw_offset(ndev, idx);
	end = pci_resource_len(ndev->ntb.pdev, bar);
	/* The DMA submap offset marks the end of an MW sharing this BAR. */
	if (ndev->dma.submap.size && ndev->dma.submap.bar == bar)
		end = min(end, ndev->dma.submap.offset);

	return offset < end ? end - offset : 0;
}

static int ntb_epf_mw_count(struct ntb_dev *ntb, int pidx)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;

	if (pidx != NTB_DEF_PEER_IDX) {
		dev_err(dev, "Unsupported Peer ID %d\n", pidx);
		return -EINVAL;
	}

	return ndev->mw_count;
}

static int ntb_epf_mw_get_align(struct ntb_dev *ntb, int pidx, int idx,
				resource_size_t *addr_align,
				resource_size_t *size_align,
				resource_size_t *size_max)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	int bar;

	if (pidx != NTB_DEF_PEER_IDX) {
		dev_err(dev, "Unsupported Peer ID %d\n", pidx);
		return -EINVAL;
	}

	bar = ntb_epf_mw_to_bar(ndev, idx);
	if (bar < 0)
		return bar;

	if (addr_align)
		*addr_align = SZ_4K;

	if (size_align)
		*size_align = 1;

	if (size_max)
		*size_max = ntb_epf_mw_size(ndev, idx, bar);

	return 0;
}

static u64 ntb_epf_link_is_up(struct ntb_dev *ntb,
			      enum ntb_speed *speed,
			      enum ntb_width *width)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	u32 status;

	status = readw(ndev->ctrl_reg + NTB_EPF_LINK_STATUS);

	return status & LINK_STATUS_UP;
}

static u32 ntb_epf_spad_read(struct ntb_dev *ntb, int idx)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	u32 offset;

	if (idx < 0 || idx >= ndev->spad_count) {
		dev_err(dev, "READ: Invalid ScratchPad Index %d\n", idx);
		return 0;
	}

	offset = readl(ndev->ctrl_reg + NTB_EPF_SPAD_OFFSET);
	offset += (idx << 2);

	return readl(ndev->ctrl_reg + offset);
}

static int ntb_epf_spad_write(struct ntb_dev *ntb,
			      int idx, u32 val)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	u32 offset;

	if (idx < 0 || idx >= ndev->spad_count) {
		dev_err(dev, "WRITE: Invalid ScratchPad Index %d\n", idx);
		return -EINVAL;
	}

	offset = readl(ndev->ctrl_reg + NTB_EPF_SPAD_OFFSET);
	offset += (idx << 2);
	writel(val, ndev->ctrl_reg + offset);

	return 0;
}

static u32 ntb_epf_peer_spad_read(struct ntb_dev *ntb, int pidx, int idx)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	u32 offset;

	if (pidx != NTB_DEF_PEER_IDX) {
		dev_err(dev, "Unsupported Peer ID %d\n", pidx);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ndev->spad_count) {
		dev_err(dev, "WRITE: Invalid Peer ScratchPad Index %d\n", idx);
		return -EINVAL;
	}

	offset = (idx << 2);
	return readl(ndev->peer_spad_reg + offset);
}

static int ntb_epf_peer_spad_write(struct ntb_dev *ntb, int pidx,
				   int idx, u32 val)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	u32 offset;

	if (pidx != NTB_DEF_PEER_IDX) {
		dev_err(dev, "Unsupported Peer ID %d\n", pidx);
		return -EINVAL;
	}

	if (idx < 0 || idx >= ndev->spad_count) {
		dev_err(dev, "WRITE: Invalid Peer ScratchPad Index %d\n", idx);
		return -EINVAL;
	}

	offset = (idx << 2);
	writel(val, ndev->peer_spad_reg + offset);

	return 0;
}

static int ntb_epf_link_enable(struct ntb_dev *ntb,
			       enum ntb_speed max_speed,
			       enum ntb_width max_width)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	int ret;

	ret = ntb_epf_send_command(ndev, CMD_LINK_UP, 0);
	if (ret) {
		dev_err(dev, "Fail to enable link\n");
		return ret;
	}

	return 0;
}

static int ntb_epf_link_disable(struct ntb_dev *ntb)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	int ret;

	ret = ntb_epf_send_command(ndev, CMD_LINK_DOWN, 0);
	if (ret) {
		dev_err(dev, "Fail to disable link\n");
		return ret;
	}

	return 0;
}

static irqreturn_t ntb_epf_vec_isr(int irq, void *dev)
{
	struct ntb_epf_irq_ctx *ctx = dev;
	struct ntb_epf_dev *ndev = ctx->ndev;
	unsigned int db_vector;
	unsigned int irq_no = ctx->irq_no;

	if (irq_no == EPF_IRQ_LINK) {
		ntb_link_event(&ndev->ntb);
	} else if (irq_no == EPF_IRQ_RESERVED_DB) {
		dev_warn_ratelimited(ndev->dev,
				     "Unexpected reserved doorbell slot IRQ received\n");
	} else {
		db_vector = irq_no - EPF_IRQ_DB_START;
		if (ndev->db_count < NTB_EPF_MIN_DB_COUNT ||
		    db_vector >= ndev->db_count - 1) {
			dev_warn_ratelimited(ndev->dev,
					     "Unexpected doorbell vector %u (db_count %u)\n",
					     db_vector, ndev->db_count);
			return IRQ_HANDLED;
		}

		atomic64_or(BIT_ULL(db_vector), &ndev->db_val);
		ntb_db_event(&ndev->ntb, db_vector);
	}

	return IRQ_HANDLED;
}

static int ntb_epf_init_isr(struct ntb_epf_dev *ndev, int msi_min, int msi_max)
{
	struct pci_dev *pdev = ndev->ntb.pdev;
	struct device *dev = ndev->dev;
	unsigned int dma_irqs = ndev->dma.nr_irqs;
	unsigned int ntb_irqs;
	u32 argument = MSIX_ENABLE;
	int irq;
	int ret;
	int i;

	irq = pci_alloc_irq_vectors(pdev, msi_min + dma_irqs,
				    msi_max + dma_irqs, PCI_IRQ_MSIX);
	if (irq < 0) {
		dev_dbg(dev, "Failed to get MSIX interrupts\n");
		irq = pci_alloc_irq_vectors(pdev, msi_min + dma_irqs,
					    msi_max + dma_irqs,
					    PCI_IRQ_MSI);
		if (irq < 0) {
			dev_err(dev, "Failed to get MSI interrupts\n");
			return irq;
		}
		argument &= ~MSIX_ENABLE;
	}

	ntb_irqs = irq - dma_irqs;
	ndev->db_count = ntb_irqs - 1;
	for (i = 0; i < ntb_irqs; i++) {
		ndev->irq_ctx[i].ndev = ndev;
		ndev->irq_ctx[i].irq_no = i;
		ret = request_irq(pci_irq_vector(pdev, i), ntb_epf_vec_isr,
				  0, "ntb_epf", &ndev->irq_ctx[i]);
		if (ret) {
			dev_err(dev, "Failed to request irq\n");
			goto err_free_irq;
		}
	}

	ret = ntb_epf_send_command(ndev, CMD_CONFIGURE_DOORBELL,
				   argument | ntb_irqs);
	if (ret) {
		dev_err(dev, "Failed to configure doorbell\n");
		goto err_free_irq;
	}

	return 0;

err_free_irq:
	while (i--)
		free_irq(pci_irq_vector(pdev, i), &ndev->irq_ctx[i]);
	pci_free_irq_vectors(pdev);

	return ret;
}

static int ntb_epf_peer_mw_count(struct ntb_dev *ntb)
{
	return ntb_ndev(ntb)->mw_count;
}

static int ntb_epf_spad_count(struct ntb_dev *ntb)
{
	return ntb_ndev(ntb)->spad_count;
}

static u64 ntb_epf_db_valid_mask(struct ntb_dev *ntb)
{
	return ntb_ndev(ntb)->db_valid_mask;
}

static int ntb_epf_db_vector_count(struct ntb_dev *ntb)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	unsigned int db_count = ndev->db_count;

	/*
	 * db_count includes an extra skipped slot due to the legacy
	 * doorbell layout. Expose only the real doorbell vectors.
	 */
	if (db_count < NTB_EPF_MIN_DB_COUNT)
		return 0;

	return db_count - 1;
}

static u64 ntb_epf_db_vector_mask(struct ntb_dev *ntb, int db_vector)
{
	int nr_vec;

	/*
	 * db_count includes one skipped slot in the legacy layout. Valid
	 * doorbell vectors are therefore [0 .. (db_count - 2)].
	 */
	nr_vec = ntb_epf_db_vector_count(ntb);
	if (db_vector < 0 || db_vector >= nr_vec)
		return 0;

	return BIT_ULL(db_vector);
}

static int ntb_epf_db_set_mask(struct ntb_dev *ntb, u64 db_bits)
{
	return 0;
}

static int ntb_epf_mw_set_trans(struct ntb_dev *ntb, int pidx, int idx,
				dma_addr_t addr, resource_size_t size)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	resource_size_t mw_size;
	int bar;

	if (pidx != NTB_DEF_PEER_IDX) {
		dev_err(dev, "Unsupported Peer ID %d\n", pidx);
		return -EINVAL;
	}

	bar = ntb_epf_mw_to_bar(ndev, idx);
	if (bar < 0)
		return bar;

	mw_size = ntb_epf_mw_size(ndev, idx, bar);

	if (size > mw_size) {
		dev_err(dev, "Size:%pa is greater than the MW size %pa\n",
			&size, &mw_size);
		return -EINVAL;
	}

	writel(lower_32_bits(addr), ndev->ctrl_reg + NTB_EPF_LOWER_ADDR);
	writel(upper_32_bits(addr), ndev->ctrl_reg + NTB_EPF_UPPER_ADDR);
	writel(lower_32_bits(size), ndev->ctrl_reg + NTB_EPF_LOWER_SIZE);
	writel(upper_32_bits(size), ndev->ctrl_reg + NTB_EPF_UPPER_SIZE);
	ntb_epf_send_command(ndev, CMD_CONFIGURE_MW, idx);

	return 0;
}

static int ntb_epf_mw_clear_trans(struct ntb_dev *ntb, int pidx, int idx)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	struct device *dev = ndev->dev;
	int ret = 0;

	ntb_epf_send_command(ndev, CMD_TEARDOWN_MW, idx);
	if (ret)
		dev_err(dev, "Failed to teardown memory window\n");

	return ret;
}

static int ntb_epf_peer_mw_get_addr(struct ntb_dev *ntb, int idx,
				    phys_addr_t *base, resource_size_t *size)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	resource_size_t offset;
	int bar;

	bar = ntb_epf_mw_to_bar(ndev, idx);
	if (bar < 0)
		return bar;

	offset = ntb_epf_mw_offset(ndev, idx);

	if (base)
		*base = pci_resource_start(ndev->ntb.pdev, bar) + offset;

	if (size)
		*size = ntb_epf_mw_size(ndev, idx, bar);

	return 0;
}

static int ntb_epf_peer_db_set(struct ntb_dev *ntb, u64 db_bits)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);
	/*
	 * ffs() returns a 1-based bit index (bit 0 -> 1).
	 *
	 * With slot 0 reserved for link events, DB#0 would naturally map to
	 * slot 1. Historically an extra +1 offset was added, so DB#0 maps to
	 * slot 2 and slot 1 remains unused. Keep this mapping for
	 * backward-compatibility.
	 */
	u32 interrupt_num = ffs(db_bits) + 1;
	struct device *dev = ndev->dev;
	u32 db_entry_size;
	u32 db_offset;
	u32 db_data;

	if (interrupt_num > ndev->db_count) {
		dev_err(dev, "DB interrupt %d greater than Max Supported %d\n",
			interrupt_num, ndev->db_count);
		return -EINVAL;
	}

	db_entry_size = readl(ndev->ctrl_reg + NTB_EPF_DB_ENTRY_SIZE);

	db_data = readl(ndev->ctrl_reg + NTB_EPF_DB_DATA(interrupt_num));
	db_offset = readl(ndev->ctrl_reg + NTB_EPF_DB_OFFSET(interrupt_num));
	writel(db_data, ndev->db_reg + (db_entry_size * interrupt_num) +
	       db_offset);

	return 0;
}

static u64 ntb_epf_db_read(struct ntb_dev *ntb)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);

	return atomic64_read(&ndev->db_val);
}

static int ntb_epf_db_clear_mask(struct ntb_dev *ntb, u64 db_bits)
{
	return 0;
}

static int ntb_epf_db_clear(struct ntb_dev *ntb, u64 db_bits)
{
	struct ntb_epf_dev *ndev = ntb_ndev(ntb);

	atomic64_and(~db_bits, &ndev->db_val);

	return 0;
}

static const struct ntb_dev_ops ntb_epf_ops = {
	.mw_count		= ntb_epf_mw_count,
	.spad_count		= ntb_epf_spad_count,
	.peer_mw_count		= ntb_epf_peer_mw_count,
	.db_valid_mask		= ntb_epf_db_valid_mask,
	.db_vector_count	= ntb_epf_db_vector_count,
	.db_vector_mask		= ntb_epf_db_vector_mask,
	.db_set_mask		= ntb_epf_db_set_mask,
	.mw_set_trans		= ntb_epf_mw_set_trans,
	.mw_clear_trans		= ntb_epf_mw_clear_trans,
	.peer_mw_get_addr	= ntb_epf_peer_mw_get_addr,
	.link_enable		= ntb_epf_link_enable,
	.spad_read		= ntb_epf_spad_read,
	.spad_write		= ntb_epf_spad_write,
	.peer_spad_read		= ntb_epf_peer_spad_read,
	.peer_spad_write	= ntb_epf_peer_spad_write,
	.peer_db_set		= ntb_epf_peer_db_set,
	.db_read		= ntb_epf_db_read,
	.mw_get_align		= ntb_epf_mw_get_align,
	.link_is_up		= ntb_epf_link_is_up,
	.db_clear_mask		= ntb_epf_db_clear_mask,
	.db_clear		= ntb_epf_db_clear,
	.link_disable		= ntb_epf_link_disable,
};

static inline void ntb_epf_init_struct(struct ntb_epf_dev *ndev,
				       struct pci_dev *pdev)
{
	ndev->ntb.pdev = pdev;
	ndev->ntb.topo = NTB_TOPO_NONE;
	ndev->ntb.ops = &ntb_epf_ops;
}

static int ntb_epf_init_dev(struct ntb_epf_dev *ndev)
{
	struct device *dev = ndev->dev;
	int ret;
	int i;

	ndev->mw_count = readl(ndev->ctrl_reg + NTB_EPF_MW_COUNT);
	if (ndev->mw_count > NTB_EPF_MAX_MW_COUNT) {
		dev_err(dev, "Unsupported MW count: %u\n", ndev->mw_count);
		return -EINVAL;
	}
	ret = ntb_epf_dma_parse(ndev);
	if (ret) {
		dev_err(dev, "Invalid endpoint DMA layout\n");
		return ret;
	}
	if (ndev->dma.submap.size) {
		for (i = 0; i < ndev->mw_count; i++) {
			int bar = ntb_epf_mw_to_bar(ndev, i);

			if (bar == ndev->dma.submap.bar &&
			    !ntb_epf_mw_size(ndev, i, bar)) {
				dev_err(dev, "Invalid DMA/MW boundary\n");
				return -EINVAL;
			}
		}
	}

	/* One Link interrupt and rest doorbell interrupt */
	ret = ntb_epf_init_isr(ndev, NTB_EPF_MIN_DB_COUNT + 1,
			       NTB_EPF_MAX_DB_COUNT + 1);
	if (ret) {
		dev_err(dev, "Failed to init ISR\n");
		return ret;
	}

	/*
	 * ndev->db_count includes an extra skipped slot due to the legacy
	 * doorbell layout, hence -1.
	 */
	ndev->db_valid_mask = BIT_ULL(ndev->db_count - 1) - 1;
	ndev->spad_count = readl(ndev->ctrl_reg + NTB_EPF_SPAD_COUNT);

	return 0;
}

static int ntb_epf_init_pci(struct ntb_epf_dev *ndev,
			    struct pci_dev *pdev)
{
	struct device *dev = ndev->dev;
	size_t spad_sz, spad_off;
	int ret;

	pci_set_drvdata(pdev, ndev);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(dev, "Cannot enable PCI device\n");
		goto err_pci_enable;
	}

	ret = pci_request_regions(pdev, "ntb");
	if (ret) {
		dev_err(dev, "Cannot obtain PCI resources\n");
		goto err_pci_regions;
	}

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(dev, "Cannot set DMA mask\n");
			goto err_pci_regions;
		}
		dev_warn(&pdev->dev, "Cannot DMA highmem\n");
	}

	ndev->ctrl_reg = pci_iomap(pdev, ndev->barno_map[BAR_CONFIG], 0);
	if (!ndev->ctrl_reg) {
		ret = -EIO;
		goto err_pci_regions;
	}

	if (ndev->barno_map[BAR_PEER_SPAD] != ndev->barno_map[BAR_CONFIG]) {
		ndev->peer_spad_reg = pci_iomap(pdev,
						ndev->barno_map[BAR_PEER_SPAD], 0);
		if (!ndev->peer_spad_reg) {
			ret = -EIO;
			goto err_pci_regions;
		}
	} else {
		spad_sz = 4 * readl(ndev->ctrl_reg + NTB_EPF_SPAD_COUNT);
		spad_off = readl(ndev->ctrl_reg + NTB_EPF_SPAD_OFFSET);
		ndev->peer_spad_reg = ndev->ctrl_reg + spad_off  + spad_sz;
	}

	ndev->db_reg = pci_iomap(pdev, ndev->barno_map[BAR_DB], 0);
	if (!ndev->db_reg) {
		ret = -EIO;
		goto err_pci_regions;
	}

	return 0;

err_pci_regions:
	pci_disable_device(pdev);

err_pci_enable:
	pci_set_drvdata(pdev, NULL);

	return ret;
}

static void ntb_epf_deinit_pci(struct ntb_epf_dev *ndev)
{
	struct pci_dev *pdev = ndev->ntb.pdev;

	pci_iounmap(pdev, ndev->ctrl_reg);
	if (ndev->barno_map[BAR_PEER_SPAD] != ndev->barno_map[BAR_CONFIG])
		pci_iounmap(pdev, ndev->peer_spad_reg);
	pci_iounmap(pdev, ndev->db_reg);

	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
}

static void ntb_epf_cleanup_isr(struct ntb_epf_dev *ndev)
{
	struct pci_dev *pdev = ndev->ntb.pdev;
	int i;

	ntb_epf_send_command(ndev, CMD_TEARDOWN_DOORBELL, ndev->db_count + 1);

	for (i = 0; i < ndev->db_count + 1; i++)
		free_irq(pci_irq_vector(pdev, i), &ndev->irq_ctx[i]);
	pci_free_irq_vectors(pdev);
}

static int ntb_epf_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct ntb_epf_dev *ndev;
	int ret;

	if (pci_is_bridge(pdev))
		return -ENODEV;

	ndev = devm_kzalloc(dev, sizeof(*ndev), GFP_KERNEL);
	if (!ndev)
		return -ENOMEM;

	ndev->barno_map = (const enum pci_barno *)id->driver_data;
	if (!ndev->barno_map)
		return -EINVAL;

	ndev->dev = dev;

	ntb_epf_init_struct(ndev, pdev);
	mutex_init(&ndev->cmd_lock);

	ret = ntb_epf_init_pci(ndev, pdev);
	if (ret) {
		dev_err(dev, "Failed to init PCI\n");
		return ret;
	}

	ret = ntb_epf_init_dev(ndev);
	if (ret) {
		dev_err(dev, "Failed to init device\n");
		goto err_init_dev;
	}

	ret = ntb_epf_dma_init(ndev);
	if (ret) {
		dev_err(dev, "Failed to initialize endpoint DMA\n");
		goto err_dma_init;
	}

	ret = ntb_register_device(&ndev->ntb);
	if (ret) {
		dev_err(dev, "Failed to register NTB device\n");
		goto err_register_dev;
	}

	return 0;

err_register_dev:
	ntb_epf_dma_deinit(ndev);
err_dma_init:
	ntb_epf_cleanup_isr(ndev);

err_init_dev:
	ntb_epf_deinit_pci(ndev);

	return ret;
}

static void ntb_epf_pci_remove(struct pci_dev *pdev)
{
	struct ntb_epf_dev *ndev = pci_get_drvdata(pdev);

	ntb_unregister_device(&ndev->ntb);
	ntb_epf_dma_deinit(ndev);
	ntb_epf_cleanup_isr(ndev);
	ntb_epf_deinit_pci(ndev);
}

static const enum pci_barno j721e_map[NTB_BAR_NUM] = {
	[BAR_CONFIG]	= BAR_0,
	[BAR_PEER_SPAD]	= BAR_1,
	[BAR_DB]	= BAR_2,
	[BAR_MW1]	= BAR_2,
	[BAR_MW2]	= BAR_3,
	[BAR_MW3]	= BAR_4,
	[BAR_MW4]	= BAR_5
};

static const enum pci_barno mx8_map[NTB_BAR_NUM] = {
	[BAR_CONFIG]	= BAR_0,
	[BAR_PEER_SPAD]	= BAR_0,
	[BAR_DB]	= BAR_2,
	[BAR_MW1]	= BAR_4,
	[BAR_MW2]	= BAR_5,
	[BAR_MW3]	= NO_BAR,
	[BAR_MW4]	= NO_BAR
};

static const enum pci_barno rcar_barno[NTB_BAR_NUM] = {
	[BAR_CONFIG]	= BAR_0,
	[BAR_PEER_SPAD]	= BAR_0,
	[BAR_DB]	= BAR_4,
	[BAR_MW1]	= BAR_2,
	[BAR_MW2]	= NO_BAR,
	[BAR_MW3]	= NO_BAR,
	[BAR_MW4]	= NO_BAR,
};

static const struct pci_device_id ntb_epf_pci_tbl[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_TI, PCI_DEVICE_ID_TI_J721E),
		.class = PCI_CLASS_MEMORY_RAM << 8, .class_mask = 0xffff00,
		.driver_data = (kernel_ulong_t)j721e_map,
	},
	{
		PCI_DEVICE(PCI_VENDOR_ID_FREESCALE, 0x0809),
		.class = PCI_CLASS_MEMORY_RAM << 8, .class_mask = 0xffff00,
		.driver_data = (kernel_ulong_t)mx8_map,
	},
	{
		PCI_DEVICE(PCI_VENDOR_ID_RENESAS, 0x0030),
		.class = PCI_CLASS_MEMORY_RAM << 8, .class_mask = 0xffff00,
		.driver_data = (kernel_ulong_t)rcar_barno,
	},
	{ },
};

static struct pci_driver ntb_epf_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= ntb_epf_pci_tbl,
	.probe		= ntb_epf_pci_probe,
	.remove		= ntb_epf_pci_remove,
};
module_pci_driver(ntb_epf_pci_driver);

MODULE_DESCRIPTION("PCI ENDPOINT NTB HOST DRIVER");
MODULE_AUTHOR("Kishon Vijay Abraham I <kishon@ti.com>");
MODULE_LICENSE("GPL v2");
