// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2019 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare eDMA PCIe driver
 *
 * Author: Gustavo Pimentel <gustavo.pimentel@synopsys.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/device.h>
#include <linux/dma/edma.h>
#include <linux/iopoll.h>
#include <linux/pci-epf.h>
#include <linux/msi.h>
#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/overflow.h>
#include <linux/pci-ep-dma.h>
#include <linux/sizes.h>

#include "dw-edma-core.h"

/* Synopsys */
#define DW_PCIE_SYNOPSYS_VSEC_DMA_ID		0x6
#define DW_PCIE_SYNOPSYS_VSEC_DMA_BAR		GENMASK(10, 8)
#define DW_PCIE_SYNOPSYS_VSEC_DMA_MAP		GENMASK(2, 0)
#define DW_PCIE_SYNOPSYS_VSEC_DMA_WR_CH		GENMASK(9, 0)
#define DW_PCIE_SYNOPSYS_VSEC_DMA_RD_CH		GENMASK(25, 16)

/* AMD MDB (Xilinx) specific defines */
#define PCI_DEVICE_ID_XILINX_B054		0xb054
#define PCI_DEVICE_ID_XILINX_B00F		0xb00f

#define DW_PCIE_XILINX_MDB_VSEC_DMA_ID		0x6
#define DW_PCIE_XILINX_MDB_VSEC_ID		0x20
#define DW_PCIE_XILINX_MDB_VSEC_DMA_BAR		GENMASK(10, 8)
#define DW_PCIE_XILINX_MDB_VSEC_DMA_MAP		GENMASK(2, 0)
#define DW_PCIE_XILINX_MDB_VSEC_DMA_WR_CH	GENMASK(9, 0)
#define DW_PCIE_XILINX_MDB_VSEC_DMA_RD_CH	GENMASK(25, 16)

#define DW_PCIE_XILINX_MDB_DEVMEM_OFF_REG_HIGH	0xc
#define DW_PCIE_XILINX_MDB_DEVMEM_OFF_REG_LOW	0x8
#define DW_PCIE_XILINX_MDB_INVALID_ADDR		(~0ULL)

#define DW_PCIE_XILINX_MDB_LL_OFF_GAP		0x200000
#define DW_PCIE_XILINX_MDB_LL_SIZE		0x800
#define DW_PCIE_XILINX_MDB_DT_OFF_GAP		0x100000
#define DW_PCIE_XILINX_MDB_DT_SIZE		0x800

#define DW_PCIE_EP_DMA_READY_POLL_US		1000
#define DW_PCIE_EP_DMA_READY_TIMEOUT_US		2000000

#define DW_BLOCK(a, b, c) \
	{ \
		.bar = a, \
		.off = b, \
		.sz = c, \
	},

struct dw_edma_block {
	enum pci_barno			bar;
	off_t				off;
	u64				paddr;
	bool				paddr_valid;
	size_t				sz;
};

struct dw_edma_pcie_data {
	/* eDMA registers location */
	struct dw_edma_block		rg;
	/* eDMA memory linked list location */
	struct dw_edma_block		ll_wr[HDMA_MAX_WR_CH];
	struct dw_edma_block		ll_rd[HDMA_MAX_RD_CH];
	/* eDMA memory data location */
	struct dw_edma_block		dt_wr[HDMA_MAX_WR_CH];
	struct dw_edma_block		dt_rd[HDMA_MAX_RD_CH];
	/* Other */
	enum dw_edma_map_format		mf;
	u8				irqs;
	u16				wr_ch_cnt;
	u16				rd_ch_cnt;
	u64				devmem_phys_off;
	bool				cfg_non_ll;
};

struct dw_edma_pcie_match_data {
	const struct dw_edma_pcie_data *data;
	const struct dw_edma_plat_ops *plat_ops;
	/*
	 * Mandatory callback. It may leave @pdata unchanged when the static
	 * template already describes the device.
	 */
	int (*parse_caps)(struct pci_dev *pdev,
			  struct dw_edma_pcie_data *pdata);
	unsigned long flags;
	u32 chip_flags;
};

#define DW_EDMA_PCIE_F_DEVMEM_PHYS_OFF	BIT(0)
#define DW_EDMA_PCIE_F_REG_OFFSET	BIT(1)

struct dw_edma_pcie_ep_dma_view {
	struct pci_dev *pdev;
	void __iomem *base;
	resource_size_t limit;
};

static const struct dw_edma_pcie_data snps_edda_data = {
	/* eDMA registers location */
	.rg.bar				= BAR_0,
	.rg.off				= 0x00001000,	/*  4 Kbytes */
	.rg.sz				= 0x00002000,	/*  8 Kbytes */
	/* eDMA memory linked list location */
	.ll_wr = {
		/* Channel 0 - BAR 2, offset 0 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00000000, 0x00000800)
		/* Channel 1 - BAR 2, offset 2 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00200000, 0x00000800)
	},
	.ll_rd = {
		/* Channel 0 - BAR 2, offset 4 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00400000, 0x00000800)
		/* Channel 1 - BAR 2, offset 6 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00600000, 0x00000800)
	},
	/* eDMA memory data location */
	.dt_wr = {
		/* Channel 0 - BAR 2, offset 8 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00800000, 0x00000800)
		/* Channel 1 - BAR 2, offset 9 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00900000, 0x00000800)
	},
	.dt_rd = {
		/* Channel 0 - BAR 2, offset 10 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00a00000, 0x00000800)
		/* Channel 1 - BAR 2, offset 11 Mbytes, size 2 Kbytes */
		DW_BLOCK(BAR_2, 0x00b00000, 0x00000800)
	},
	/* Other */
	.mf				= EDMA_MF_EDMA_UNROLL,
	.irqs				= 1,
	.wr_ch_cnt			= 2,
	.rd_ch_cnt			= 2,
};

static const struct dw_edma_pcie_data xilinx_mdb_data = {
	/* MDB registers location */
	.rg.bar				= BAR_0,
	.rg.off				= SZ_4K,	/*  4 Kbytes */
	.rg.sz				= SZ_8K,	/*  8 Kbytes */

	/* Other */
	.mf				= EDMA_MF_HDMA_NATIVE,
	.irqs				= 1,
	.wr_ch_cnt			= 8,
	.rd_ch_cnt			= 8,
};

static const struct dw_edma_pcie_data xilinx_cpm6_dma_data = {
	/* MDB registers location */
	.rg.bar				= BAR_0,
	.rg.off				= SZ_4K,	/*  4 Kbytes */
	.rg.sz				= SZ_8K,	/*  8 Kbytes */

	/* Other */
	.mf				= EDMA_MF_HDMA_NATIVE,
	.irqs				= 1,
	.wr_ch_cnt			= 8,
	.rd_ch_cnt			= 8,
};

static const struct dw_edma_pcie_data ep_dma_data = {
	.mf				= EDMA_MF_EDMA_UNROLL,
	.irqs				= EDMA_MAX_WR_CH + EDMA_MAX_RD_CH,
	.wr_ch_cnt			= EDMA_MAX_WR_CH,
	.rd_ch_cnt			= EDMA_MAX_RD_CH,
};

static void dw_edma_set_chan_region_offset(struct dw_edma_pcie_data *pdata,
					   enum pci_barno bar, off_t start_off,
					   off_t ll_off_gap, size_t ll_size,
					   off_t dt_off_gap, size_t dt_size)
{
	u16 wr_ch = pdata->wr_ch_cnt;
	u16 rd_ch = pdata->rd_ch_cnt;
	off_t off;
	u16 i;

	off = start_off;

	/* Write channel LL region */
	for (i = 0; i < wr_ch; i++) {
		pdata->ll_wr[i].bar = bar;
		pdata->ll_wr[i].off = off;
		pdata->ll_wr[i].sz = ll_size;
		off += ll_off_gap;
	}

	/* Read channel LL region */
	for (i = 0; i < rd_ch; i++) {
		pdata->ll_rd[i].bar = bar;
		pdata->ll_rd[i].off = off;
		pdata->ll_rd[i].sz = ll_size;
		off += ll_off_gap;
	}

	/* Write channel data region */
	for (i = 0; i < wr_ch; i++) {
		pdata->dt_wr[i].bar = bar;
		pdata->dt_wr[i].off = off;
		pdata->dt_wr[i].sz = dt_size;
		off += dt_off_gap;
	}

	/* Read channel data region */
	for (i = 0; i < rd_ch; i++) {
		pdata->dt_rd[i].bar = bar;
		pdata->dt_rd[i].off = off;
		pdata->dt_rd[i].sz = dt_size;
		off += dt_off_gap;
	}
}

static int dw_edma_pcie_irq_vector(struct device *dev, unsigned int nr)
{
	return pci_irq_vector(to_pci_dev(dev), nr);
}

static u64 dw_edma_pcie_address(struct device *dev, phys_addr_t cpu_addr)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct pci_bus_region region;
	struct resource res = {
		.flags = IORESOURCE_MEM,
		.start = cpu_addr,
		.end = cpu_addr,
	};

	pcibios_resource_to_bus(pdev->bus, &region, &res);
	return region.start;
}

static const struct dw_edma_plat_ops dw_edma_pcie_plat_ops = {
	.irq_vector = dw_edma_pcie_irq_vector,
	.pci_address = dw_edma_pcie_address,
};

static const struct dw_edma_plat_ops dw_edma_pcie_raw_addr_plat_ops = {
	.irq_vector = dw_edma_pcie_irq_vector,
};

static bool dw_edma_pcie_valid_bar(enum pci_barno bar)
{
	return bar >= BAR_0 && bar <= BAR_5;
}

static bool dw_edma_pcie_valid_bar_range(struct pci_dev *pdev,
					 enum pci_barno bar, u64 off,
					 size_t sz)
{
	resource_size_t bar_len;

	if (!dw_edma_pcie_valid_bar(bar) || !sz)
		return false;

	bar_len = pci_resource_len(pdev, bar);

	return off <= bar_len && sz <= bar_len - off;
}

static bool dw_edma_pcie_valid_block(struct pci_dev *pdev,
				     const struct dw_edma_block *block)
{
	return dw_edma_pcie_valid_bar_range(pdev, block->bar, block->off,
					    block->sz);
}

static bool dw_edma_pcie_ep_dma_bar_scannable(struct pci_dev *pdev,
					      enum pci_barno bar)
{
	unsigned long flags = pci_resource_flags(pdev, bar);

	if (!(flags & IORESOURCE_MEM))
		return false;

	if (flags & (IORESOURCE_UNSET | IORESOURCE_DISABLED))
		return false;

	return pci_resource_len(pdev, bar) >= PCI_EP_DMA_METADATA_HDR_LEN;
}

static u32 dw_edma_pcie_ep_dma_readl(struct dw_edma_pcie_ep_dma_view *view,
				     u16 off)
{
	return readl(view->base + off);
}

static void dw_edma_pcie_ep_dma_writel(struct dw_edma_pcie_ep_dma_view *view,
				       u16 off, u32 val)
{
	writel(val, view->base + off);
}

static void
dw_edma_pcie_ep_dma_clear_host_req(struct dw_edma_pcie_ep_dma_view *view)
{
	u32 ctrl;

	ctrl = dw_edma_pcie_ep_dma_readl(view, PCI_EP_DMA_METADATA_CTRL);
	ctrl &= ~PCI_EP_DMA_METADATA_CTRL_HOST_REQ;
	dw_edma_pcie_ep_dma_writel(view, PCI_EP_DMA_METADATA_CTRL, ctrl);
}

static u64 dw_edma_pcie_ep_dma_read64(struct dw_edma_pcie_ep_dma_view *view,
				      u16 lo, u16 hi)
{
	u64 val;

	val = dw_edma_pcie_ep_dma_readl(view, hi);

	return (val << 32) | dw_edma_pcie_ep_dma_readl(view, lo);
}

static int dw_edma_pcie_ep_dma_read_off(struct dw_edma_pcie_ep_dma_view *view,
					u16 lo, u16 hi, off_t *off)
{
	u64 val;

	val = dw_edma_pcie_ep_dma_read64(view, lo, hi);
	if (val > type_max(*off))
		return -EINVAL;

	*off = val;

	return 0;
}

static void dw_edma_pcie_get_synopsys_dma_data(struct pci_dev *pdev,
					       struct dw_edma_pcie_data *pdata)
{
	u32 val, map;
	u16 vsec;
	u64 off;

	vsec = pci_find_vsec_capability(pdev, PCI_VENDOR_ID_SYNOPSYS,
					DW_PCIE_SYNOPSYS_VSEC_DMA_ID);
	if (!vsec)
		return;

	pci_read_config_dword(pdev, vsec + PCI_VNDR_HEADER, &val);
	if (PCI_VNDR_HEADER_REV(val) != 0x00 ||
	    PCI_VNDR_HEADER_LEN(val) != 0x18)
		return;

	pci_dbg(pdev, "Detected Synopsys PCIe Vendor-Specific Extended Capability DMA\n");
	pci_read_config_dword(pdev, vsec + 0x8, &val);
	map = FIELD_GET(DW_PCIE_SYNOPSYS_VSEC_DMA_MAP, val);
	if (map != EDMA_MF_EDMA_LEGACY &&
	    map != EDMA_MF_EDMA_UNROLL &&
	    map != EDMA_MF_HDMA_COMPAT &&
	    map != EDMA_MF_HDMA_NATIVE)
		return;

	pdata->mf = map;
	pdata->rg.bar = FIELD_GET(DW_PCIE_SYNOPSYS_VSEC_DMA_BAR, val);

	pci_read_config_dword(pdev, vsec + 0xc, &val);
	pdata->wr_ch_cnt = min_t(u16, pdata->wr_ch_cnt,
				 FIELD_GET(DW_PCIE_SYNOPSYS_VSEC_DMA_WR_CH, val));
	pdata->rd_ch_cnt = min_t(u16, pdata->rd_ch_cnt,
				 FIELD_GET(DW_PCIE_SYNOPSYS_VSEC_DMA_RD_CH, val));

	pci_read_config_dword(pdev, vsec + 0x14, &val);
	off = val;
	pci_read_config_dword(pdev, vsec + 0x10, &val);
	off <<= 32;
	off |= val;
	pdata->rg.off = off;
}

static void dw_edma_pcie_get_xilinx_dma_data(struct pci_dev *pdev,
					     struct dw_edma_pcie_data *pdata)
{
	u32 val, map;
	u16 vsec;
	u64 off;

	pdata->devmem_phys_off = DW_PCIE_XILINX_MDB_INVALID_ADDR;

	vsec = pci_find_vsec_capability(pdev, PCI_VENDOR_ID_XILINX,
					DW_PCIE_XILINX_MDB_VSEC_DMA_ID);
	if (!vsec)
		return;

	pci_read_config_dword(pdev, vsec + PCI_VNDR_HEADER, &val);
	if (PCI_VNDR_HEADER_REV(val) != 0x00 ||
	    PCI_VNDR_HEADER_LEN(val) != 0x18)
		return;

	pci_dbg(pdev, "Detected Xilinx PCIe Vendor-Specific Extended Capability DMA\n");
	pci_read_config_dword(pdev, vsec + 0x8, &val);
	map = FIELD_GET(DW_PCIE_XILINX_MDB_VSEC_DMA_MAP, val);
	if (map != EDMA_MF_HDMA_NATIVE)
		return;

	pdata->mf = map;
	pdata->rg.bar = FIELD_GET(DW_PCIE_XILINX_MDB_VSEC_DMA_BAR, val);

	pci_read_config_dword(pdev, vsec + 0xc, &val);
	pdata->wr_ch_cnt = min(pdata->wr_ch_cnt,
			       FIELD_GET(DW_PCIE_XILINX_MDB_VSEC_DMA_WR_CH, val));
	pdata->rd_ch_cnt = min(pdata->rd_ch_cnt,
			       FIELD_GET(DW_PCIE_XILINX_MDB_VSEC_DMA_RD_CH, val));

	pci_read_config_dword(pdev, vsec + 0x14, &val);
	off = val;
	pci_read_config_dword(pdev, vsec + 0x10, &val);
	off <<= 32;
	off |= val;
	pdata->rg.off = off;

	vsec = pci_find_vsec_capability(pdev, PCI_VENDOR_ID_XILINX,
					DW_PCIE_XILINX_MDB_VSEC_ID);
	if (!vsec)
		return;

	pci_read_config_dword(pdev,
			      vsec + DW_PCIE_XILINX_MDB_DEVMEM_OFF_REG_HIGH,
			      &val);
	off = val;
	pci_read_config_dword(pdev,
			      vsec + DW_PCIE_XILINX_MDB_DEVMEM_OFF_REG_LOW,
			      &val);
	off <<= 32;
	off |= val;
	pdata->devmem_phys_off = off;
}

static int
dw_edma_pcie_parse_ep_dma_ch_table(struct dw_edma_pcie_ep_dma_view *view,
				   struct dw_edma_pcie_data *pdata,
				   u16 table_off, u16 entry_size, u16 ch_cnt,
				   bool write)
{
	struct dw_edma_block *desc_blocks = write ? pdata->ll_wr : pdata->ll_rd;
	struct dw_edma_block *data_blocks = write ? pdata->dt_wr : pdata->dt_rd;
	u32 ctrl;
	u16 i;
	int ret;

	for (i = 0; i < ch_cnt; i++) {
		struct dw_edma_block *desc_block = &desc_blocks[i];
		struct dw_edma_block *data_block = &data_blocks[i];
		u16 off = table_off + i * entry_size;
		u16 field, lo, hi;

		field = off + PCI_EP_DMA_METADATA_CH_CTRL;
		ctrl = dw_edma_pcie_ep_dma_readl(view, field);
		if (FIELD_GET(PCI_EP_DMA_METADATA_CH_CTRL_HW_CH, ctrl) != i)
			return -EOPNOTSUPP;

		desc_block->bar =
			FIELD_GET(PCI_EP_DMA_METADATA_CH_CTRL_DESC_BAR, ctrl);
		lo = off + PCI_EP_DMA_METADATA_CH_DESC_OFF_LO;
		hi = off + PCI_EP_DMA_METADATA_CH_DESC_OFF_HI;
		ret = dw_edma_pcie_ep_dma_read_off(view, lo, hi,
						   &desc_block->off);
		if (ret)
			return ret;
		field = off + PCI_EP_DMA_METADATA_CH_DESC_SIZE;
		desc_block->sz = dw_edma_pcie_ep_dma_readl(view, field);
		lo = off + PCI_EP_DMA_METADATA_CH_DESC_ADDR_LO;
		hi = off + PCI_EP_DMA_METADATA_CH_DESC_ADDR_HI;
		desc_block->paddr =
			dw_edma_pcie_ep_dma_read64(view, lo, hi);
		desc_block->paddr_valid = true;
		if (!dw_edma_pcie_valid_block(view->pdev, desc_block))
			return -EINVAL;

		*data_block = (struct dw_edma_block) { .bar = NO_BAR };
		if (!(ctrl & PCI_EP_DMA_METADATA_CH_CTRL_AUX_VALID))
			continue;

		data_block->bar =
			FIELD_GET(PCI_EP_DMA_METADATA_CH_CTRL_AUX_BAR, ctrl);
		lo = off + PCI_EP_DMA_METADATA_CH_AUX_OFF_LO;
		hi = off + PCI_EP_DMA_METADATA_CH_AUX_OFF_HI;
		ret = dw_edma_pcie_ep_dma_read_off(view, lo, hi,
						   &data_block->off);
		if (ret)
			return ret;
		field = off + PCI_EP_DMA_METADATA_CH_AUX_SIZE;
		data_block->sz = dw_edma_pcie_ep_dma_readl(view, field);
		lo = off + PCI_EP_DMA_METADATA_CH_AUX_ADDR_LO;
		hi = off + PCI_EP_DMA_METADATA_CH_AUX_ADDR_HI;
		data_block->paddr =
			dw_edma_pcie_ep_dma_read64(view, lo, hi);
		data_block->paddr_valid = true;
		if (!dw_edma_pcie_valid_block(view->pdev, data_block))
			return -EINVAL;
	}

	return 0;
}

static int
dw_edma_pcie_ep_dma_wait_ready(struct dw_edma_pcie_ep_dma_view *view)
{
	u32 val;

	/*
	 * The host cannot build a usable eDMA instance until the endpoint has
	 * pinned and published the channel submaps, so keep the handshake
	 * synchronous and bounded during probe.
	 */
	return read_poll_timeout(dw_edma_pcie_ep_dma_readl, val,
				 val & PCI_EP_DMA_METADATA_CTRL_READY,
				 DW_PCIE_EP_DMA_READY_POLL_US,
				 DW_PCIE_EP_DMA_READY_TIMEOUT_US, false,
				 view, PCI_EP_DMA_METADATA_CTRL);
}

static int
dw_edma_pcie_validate_ep_dma_metadata(struct dw_edma_pcie_ep_dma_view *view,
				      u32 *metadata_ctrl, u8 *reg_layout_data)
{
	size_t table_size, table_end;
	enum pci_barno reg_bar;
	u16 len, entry_size;
	u16 wr_ch_cnt, rd_ch_cnt;
	u8 layout, layout_data;
	u32 val;

	val = dw_edma_pcie_ep_dma_readl(view, 0);
	if (val != PCI_EP_DMA_METADATA_MAGIC)
		return -ENODEV;

	val = dw_edma_pcie_ep_dma_readl(view, PCI_EP_DMA_METADATA_HDR);
	if (FIELD_GET(PCI_EP_DMA_METADATA_HDR_REV, val) !=
	    PCI_EP_DMA_METADATA_REV)
		return -EINVAL;

	len = FIELD_GET(PCI_EP_DMA_METADATA_HDR_LEN_FIELD, val);
	if (len < PCI_EP_DMA_METADATA_HDR_LEN)
		return -EINVAL;
	if (len > view->limit)
		return -EINVAL;

	val = dw_edma_pcie_ep_dma_readl(view, PCI_EP_DMA_METADATA_REG_LAYOUT);
	layout = FIELD_GET(PCI_EP_DMA_METADATA_REG_LAYOUT_ID, val);
	if (layout != PCI_EP_DMA_METADATA_REG_LAYOUT_DW_EDMA)
		return -EOPNOTSUPP;

	layout_data = FIELD_GET(PCI_EP_DMA_METADATA_REG_LAYOUT_DATA, val);
	if (layout_data == EDMA_MF_EDMA_LEGACY)
		return -EOPNOTSUPP;
	if (layout_data != EDMA_MF_EDMA_UNROLL &&
	    layout_data != EDMA_MF_HDMA_COMPAT &&
	    layout_data != EDMA_MF_HDMA_NATIVE)
		return -EINVAL;

	val = dw_edma_pcie_ep_dma_readl(view, PCI_EP_DMA_METADATA_CTRL);
	reg_bar = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_REG_BAR, val);
	if (!dw_edma_pcie_valid_bar(reg_bar))
		return -EINVAL;

	wr_ch_cnt = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_WR_CH_COUNT, val);
	rd_ch_cnt = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_RD_CH_COUNT, val);
	if (!wr_ch_cnt && !rd_ch_cnt)
		return -EINVAL;
	if (wr_ch_cnt > EDMA_MAX_WR_CH || rd_ch_cnt > EDMA_MAX_RD_CH)
		return -EINVAL;

	entry_size = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_CH_ENTRY_SIZE, val);
	if (entry_size < PCI_EP_DMA_METADATA_CH_ENTRY_SIZE ||
	    entry_size % sizeof(u32))
		return -EINVAL;

	if (check_mul_overflow((size_t)(wr_ch_cnt + rd_ch_cnt),
			       (size_t)entry_size, &table_size) ||
	    check_add_overflow((size_t)PCI_EP_DMA_METADATA_HDR_LEN,
			       table_size, &table_end) ||
	    table_end > len)
		return -EINVAL;

	if (metadata_ctrl)
		*metadata_ctrl = val;
	if (reg_layout_data)
		*reg_layout_data = layout_data;

	return 0;
}

static int
dw_edma_pcie_parse_ep_dma_data(struct dw_edma_pcie_ep_dma_view *view,
			       struct dw_edma_pcie_data *pdata)
{
	u32 ctrl, reg_sz;
	u8 reg_layout_data;
	u64 reg_off;
	u16 wr_table, rd_table, entry_size;
	u16 wr_ch_cnt, rd_ch_cnt;
	int ret;

	ret = dw_edma_pcie_validate_ep_dma_metadata(view, &ctrl,
						    &reg_layout_data);
	if (ret)
		return ret;

	pci_dbg(view->pdev, "Detected PCI endpoint DMA BAR metadata\n");

	pdata->mf = reg_layout_data;
	pdata->rg.bar = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_REG_BAR, ctrl);

	wr_ch_cnt = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_WR_CH_COUNT, ctrl);
	rd_ch_cnt = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_RD_CH_COUNT, ctrl);
	pdata->wr_ch_cnt = min_t(u16, pdata->wr_ch_cnt, wr_ch_cnt);
	pdata->rd_ch_cnt = min_t(u16, pdata->rd_ch_cnt, rd_ch_cnt);
	pdata->irqs = pdata->wr_ch_cnt + pdata->rd_ch_cnt;
	reg_off = dw_edma_pcie_ep_dma_read64(view,
					     PCI_EP_DMA_METADATA_REG_OFF_LO,
					     PCI_EP_DMA_METADATA_REG_OFF_HI);
	reg_sz = dw_edma_pcie_ep_dma_readl(view, PCI_EP_DMA_METADATA_REG_SIZE);
	if (reg_off > type_max(pdata->rg.off) ||
	    !dw_edma_pcie_valid_bar_range(view->pdev, pdata->rg.bar,
					  reg_off, reg_sz))
		return -EINVAL;
	pdata->rg.off = reg_off;
	pdata->rg.sz = reg_sz;

	entry_size = FIELD_GET(PCI_EP_DMA_METADATA_CTRL_CH_ENTRY_SIZE, ctrl);
	wr_table = PCI_EP_DMA_METADATA_HDR_LEN;
	rd_table = PCI_EP_DMA_METADATA_HDR_LEN + wr_ch_cnt * entry_size;

	ret = dw_edma_pcie_parse_ep_dma_ch_table(view, pdata, wr_table,
						 entry_size, pdata->wr_ch_cnt,
						 true);
	if (ret)
		return ret;

	return dw_edma_pcie_parse_ep_dma_ch_table(view, pdata, rd_table,
						  entry_size,
						  pdata->rd_ch_cnt, false);
}

static int
dw_edma_pcie_parse_ep_dma_caps(struct pci_dev *pdev,
			       struct dw_edma_pcie_data *pdata)
{
	struct dw_edma_pcie_ep_dma_view metadata_view;
	void __iomem *base;
	resource_size_t bar_len;
	enum pci_barno bar;
	u32 ctrl;
	int ret;

	for (bar = BAR_0; bar < PCI_STD_NUM_BARS; bar++) {
		if (!dw_edma_pcie_ep_dma_bar_scannable(pdev, bar))
			continue;

		bar_len = pci_resource_len(pdev, bar);
		base = pci_iomap_range(pdev, bar, 0, 0);
		if (!base)
			continue;

		metadata_view = (struct dw_edma_pcie_ep_dma_view) {
			.pdev = pdev,
			.base = base,
			.limit = bar_len,
		};
		ret = dw_edma_pcie_validate_ep_dma_metadata(&metadata_view,
							    NULL, NULL);
		if (ret == -ENODEV) {
			pci_iounmap(metadata_view.pdev, base);
			continue;
		}
		if (ret) {
			pci_iounmap(metadata_view.pdev, base);
			return ret;
		}

		ctrl = dw_edma_pcie_ep_dma_readl(&metadata_view,
						 PCI_EP_DMA_METADATA_CTRL);
		ctrl |= PCI_EP_DMA_METADATA_CTRL_HOST_REQ;
		dw_edma_pcie_ep_dma_writel(&metadata_view,
					   PCI_EP_DMA_METADATA_CTRL, ctrl);

		ret = dw_edma_pcie_ep_dma_wait_ready(&metadata_view);
		if (ret) {
			dw_edma_pcie_ep_dma_clear_host_req(&metadata_view);
			pci_iounmap(metadata_view.pdev, base);
			return ret;
		}

		ret = dw_edma_pcie_parse_ep_dma_data(&metadata_view, pdata);
		if (ret)
			dw_edma_pcie_ep_dma_clear_host_req(&metadata_view);
		pci_iounmap(metadata_view.pdev, base);

		return ret;
	}

	return -ENODEV;
}

static int
dw_edma_pcie_parse_synopsys_caps(struct pci_dev *pdev,
				 struct dw_edma_pcie_data *pdata)
{
	dw_edma_pcie_get_synopsys_dma_data(pdev, pdata);

	return 0;
}

static int
dw_edma_pcie_parse_xilinx_caps(struct pci_dev *pdev,
			       struct dw_edma_pcie_data *pdata)
{
	dw_edma_pcie_get_xilinx_dma_data(pdev, pdata);

	/*
	 * There is no valid address found for the LL memory space on the
	 * device side. In the absence of LL base address use the non-LL mode or
	 * simple mode supported by the HDMA IP.
	 */
	if (pdata->devmem_phys_off == DW_PCIE_XILINX_MDB_INVALID_ADDR) {
		pdata->cfg_non_ll = true;
		return 0;
	}

	/*
	 * Configure the channel LL and data blocks if number of channels
	 * enabled in VSEC capability are more than the channels configured in
	 * xilinx_mdb_data.
	 */
	dw_edma_set_chan_region_offset(pdata, BAR_2, 0,
				       DW_PCIE_XILINX_MDB_LL_OFF_GAP,
				       DW_PCIE_XILINX_MDB_LL_SIZE,
				       DW_PCIE_XILINX_MDB_DT_OFF_GAP,
				       DW_PCIE_XILINX_MDB_DT_SIZE);

	return 0;
}

static const struct dw_edma_pcie_match_data ep_dma_match_data = {
	.data = &ep_dma_data,
	.plat_ops = &dw_edma_pcie_raw_addr_plat_ops,
	.parse_caps = dw_edma_pcie_parse_ep_dma_caps,
	.flags = DW_EDMA_PCIE_F_REG_OFFSET,
	.chip_flags = DW_EDMA_CHIP_PARTIAL,
};

static u64 dw_edma_get_phys_addr(struct pci_dev *pdev,
				 const struct dw_edma_pcie_match_data *match,
				 struct dw_edma_pcie_data *pdata,
				 enum pci_barno bar)
{
	if (match->flags & DW_EDMA_PCIE_F_DEVMEM_PHYS_OFF)
		return pdata->devmem_phys_off;

	return pci_bus_address(pdev, bar);
}

static u64 dw_edma_get_block_addr(struct pci_dev *pdev,
				  const struct dw_edma_pcie_match_data *match,
				  struct dw_edma_pcie_data *pdata,
				  const struct dw_edma_block *block)
{
	if (block->paddr_valid)
		return block->paddr;

	return dw_edma_get_phys_addr(pdev, match, pdata, block->bar) +
	       block->off;
}

static int dw_edma_pcie_probe(struct pci_dev *pdev,
			      const struct pci_device_id *pid)
{
	const struct dw_edma_pcie_match_data *match = (void *)pid->driver_data;
	const struct dw_edma_pcie_data *pdata;
	struct device *dev = &pdev->dev;
	struct dw_edma_chip *chip;
	int err, nr_irqs;
	int i, mask;

	if (!match) {
		/*
		 * The endpoint DMA metadata path has no static PCI ID yet.
		 * Accept it only for an explicit driver_override bind, not for
		 * arbitrary dynamic IDs without driver data.
		 */
		if (!device_has_driver_override(&pdev->dev))
			return -ENODEV;

		match = &ep_dma_match_data;
	}
	pdata = match->data;

	if (!pdata)
		return -ENODEV;

	struct dw_edma_pcie_data *dma_data __free(kfree) =
		kmemdup(pdata, sizeof(*dma_data), GFP_KERNEL);
	if (!dma_data)
		return -ENOMEM;

	/* Enable PCI device */
	err = pcim_enable_device(pdev);
	if (err) {
		pci_err(pdev, "enabling device failed\n");
		return err;
	}

	/* Let device-specific discovery override the static template data. */
	if (!match->parse_caps || !match->plat_ops)
		return -EINVAL;

	err = match->parse_caps(pdev, dma_data);
	if (err)
		return err;

	/* Mapping PCI BAR regions */
	mask = BIT(dma_data->rg.bar);
	for (i = 0; i < dma_data->wr_ch_cnt; i++) {
		mask |= BIT(dma_data->ll_wr[i].bar);
		if (dma_data->dt_wr[i].sz)
			mask |= BIT(dma_data->dt_wr[i].bar);
	}
	for (i = 0; i < dma_data->rd_ch_cnt; i++) {
		mask |= BIT(dma_data->ll_rd[i].bar);
		if (dma_data->dt_rd[i].sz)
			mask |= BIT(dma_data->dt_rd[i].bar);
	}
	err = pcim_iomap_regions(pdev, mask, pci_name(pdev));
	if (err) {
		pci_err(pdev, "eDMA BAR I/O remapping failed\n");
		return err;
	}

	pci_set_master(pdev);

	/* DMA configuration */
	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		pci_err(pdev, "DMA mask 64 set failed\n");
		return err;
	}

	/* Data structure allocation */
	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	/* IRQs allocation */
	nr_irqs = pci_alloc_irq_vectors(pdev, 1, dma_data->irqs,
					PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (nr_irqs < 1) {
		pci_err(pdev, "fail to alloc IRQ vector (number of IRQs=%u)\n",
			nr_irqs);
		return -EPERM;
	}

	/* Data structure initialization */
	chip->dev = dev;

	chip->mf = dma_data->mf;
	chip->flags = match->chip_flags;
	chip->func_no = PCI_FUNC(pdev->devfn);
	chip->nr_irqs = nr_irqs;
	chip->ops = match->plat_ops;
	chip->cfg_non_ll = dma_data->cfg_non_ll;

	chip->ll_wr_cnt = dma_data->wr_ch_cnt;
	chip->ll_rd_cnt = dma_data->rd_ch_cnt;

	chip->reg_base = pcim_iomap_table(pdev)[dma_data->rg.bar];
	if (!chip->reg_base)
		return -ENOMEM;
	if (match->flags & DW_EDMA_PCIE_F_REG_OFFSET)
		chip->reg_base += dma_data->rg.off;

	for (i = 0; i < chip->ll_wr_cnt && !dma_data->cfg_non_ll; i++) {
		struct dw_edma_region *ll_region = &chip->ll_region_wr[i];
		struct dw_edma_region *dt_region = &chip->dt_region_wr[i];
		struct dw_edma_block *ll_block = &dma_data->ll_wr[i];
		struct dw_edma_block *dt_block = &dma_data->dt_wr[i];

		ll_region->vaddr.io = pcim_iomap_table(pdev)[ll_block->bar];
		if (!ll_region->vaddr.io)
			return -ENOMEM;

		ll_region->vaddr.io += ll_block->off;
		ll_region->paddr = dw_edma_get_block_addr(pdev, match, dma_data,
							  ll_block);
		ll_region->sz = ll_block->sz;

		if (!dt_block->sz)
			continue;

		dt_region->vaddr.io = pcim_iomap_table(pdev)[dt_block->bar];
		if (!dt_region->vaddr.io)
			return -ENOMEM;

		dt_region->vaddr.io += dt_block->off;
		dt_region->paddr = dw_edma_get_block_addr(pdev, match, dma_data,
							  dt_block);
		dt_region->sz = dt_block->sz;
	}

	for (i = 0; i < chip->ll_rd_cnt && !dma_data->cfg_non_ll; i++) {
		struct dw_edma_region *ll_region = &chip->ll_region_rd[i];
		struct dw_edma_region *dt_region = &chip->dt_region_rd[i];
		struct dw_edma_block *ll_block = &dma_data->ll_rd[i];
		struct dw_edma_block *dt_block = &dma_data->dt_rd[i];

		ll_region->vaddr.io = pcim_iomap_table(pdev)[ll_block->bar];
		if (!ll_region->vaddr.io)
			return -ENOMEM;

		ll_region->vaddr.io += ll_block->off;
		ll_region->paddr = dw_edma_get_block_addr(pdev, match, dma_data,
							  ll_block);
		ll_region->sz = ll_block->sz;

		if (!dt_block->sz)
			continue;

		dt_region->vaddr.io = pcim_iomap_table(pdev)[dt_block->bar];
		if (!dt_region->vaddr.io)
			return -ENOMEM;

		dt_region->vaddr.io += dt_block->off;
		dt_region->paddr = dw_edma_get_block_addr(pdev, match, dma_data,
							  dt_block);
		dt_region->sz = dt_block->sz;
	}

	/* Debug info */
	if (chip->mf == EDMA_MF_EDMA_LEGACY)
		pci_dbg(pdev, "Version:\teDMA Port Logic (0x%x)\n", chip->mf);
	else if (chip->mf == EDMA_MF_EDMA_UNROLL)
		pci_dbg(pdev, "Version:\teDMA Unroll (0x%x)\n", chip->mf);
	else if (chip->mf == EDMA_MF_HDMA_COMPAT)
		pci_dbg(pdev, "Version:\tHDMA Compatible (0x%x)\n", chip->mf);
	else if (chip->mf == EDMA_MF_HDMA_NATIVE)
		pci_dbg(pdev, "Version:\tHDMA Native (0x%x)\n", chip->mf);
	else
		pci_dbg(pdev, "Version:\tUnknown (0x%x)\n", chip->mf);

	pci_dbg(pdev, "Registers:\tBAR=%u, off=0x%.8lx, sz=0x%zx bytes, addr(v=%p)\n",
		dma_data->rg.bar, dma_data->rg.off, dma_data->rg.sz,
		chip->reg_base);


	for (i = 0; i < chip->ll_wr_cnt; i++) {
		pci_dbg(pdev, "L. List:\tWRITE CH%.2u, BAR=%u, off=0x%.8lx, sz=0x%zx bytes, addr(v=%p, p=%pa)\n",
			i, dma_data->ll_wr[i].bar,
			dma_data->ll_wr[i].off, chip->ll_region_wr[i].sz,
			chip->ll_region_wr[i].vaddr.io, &chip->ll_region_wr[i].paddr);

		if (!dma_data->dt_wr[i].sz)
			continue;

		pci_dbg(pdev, "Data:\tWRITE CH%.2u, BAR=%u, off=0x%.8lx, sz=0x%zx bytes, addr(v=%p, p=%pa)\n",
			i, dma_data->dt_wr[i].bar,
			dma_data->dt_wr[i].off, chip->dt_region_wr[i].sz,
			chip->dt_region_wr[i].vaddr.io,
			&chip->dt_region_wr[i].paddr);
	}

	for (i = 0; i < chip->ll_rd_cnt; i++) {
		pci_dbg(pdev, "L. List:\tREAD CH%.2u, BAR=%u, off=0x%.8lx, sz=0x%zx bytes, addr(v=%p, p=%pa)\n",
			i, dma_data->ll_rd[i].bar,
			dma_data->ll_rd[i].off, chip->ll_region_rd[i].sz,
			chip->ll_region_rd[i].vaddr.io, &chip->ll_region_rd[i].paddr);

		if (!dma_data->dt_rd[i].sz)
			continue;

		pci_dbg(pdev, "Data:\tREAD CH%.2u, BAR=%u, off=0x%.8lx, sz=0x%zx bytes, addr(v=%p, p=%pa)\n",
			i, dma_data->dt_rd[i].bar,
			dma_data->dt_rd[i].off, chip->dt_region_rd[i].sz,
			chip->dt_region_rd[i].vaddr.io,
			&chip->dt_region_rd[i].paddr);
	}

	pci_dbg(pdev, "Nr. IRQs:\t%u\n", chip->nr_irqs);

	/* Validating if PCI interrupts were enabled */
	if (!pci_dev_msi_enabled(pdev)) {
		pci_err(pdev, "enable interrupt failed\n");
		return -EPERM;
	}

	/* Starting eDMA driver */
	err = dw_edma_probe(chip);
	if (err) {
		pci_err(pdev, "eDMA probe failed\n");
		return err;
	}

	/* Saving data structure reference */
	pci_set_drvdata(pdev, chip);

	return 0;
}

static void dw_edma_pcie_remove(struct pci_dev *pdev)
{
	struct dw_edma_chip *chip = pci_get_drvdata(pdev);
	int err;

	/* Stopping eDMA driver */
	err = dw_edma_remove(chip);
	if (err)
		pci_warn(pdev, "can't remove device properly: %d\n", err);
}

static const struct dw_edma_pcie_match_data snps_edda_match_data = {
	.data = &snps_edda_data,
	.plat_ops = &dw_edma_pcie_plat_ops,
	.parse_caps = dw_edma_pcie_parse_synopsys_caps,
};

static const struct dw_edma_pcie_match_data xilinx_mdb_match_data = {
	.data = &xilinx_mdb_data,
	.plat_ops = &dw_edma_pcie_plat_ops,
	.parse_caps = dw_edma_pcie_parse_xilinx_caps,
	.flags = DW_EDMA_PCIE_F_DEVMEM_PHYS_OFF,
};

static const struct dw_edma_pcie_match_data xilinx_cpm6_dma_match_data = {
	.data = &xilinx_cpm6_dma_data,
	.plat_ops = &dw_edma_pcie_plat_ops,
	.parse_caps = dw_edma_pcie_parse_xilinx_caps,
	.flags = DW_EDMA_PCIE_F_DEVMEM_PHYS_OFF,
};

static const struct pci_device_id dw_edma_pcie_id_table[] = {
	{ PCI_DEVICE_DATA(SYNOPSYS, EDDA, &snps_edda_match_data) },
	{ PCI_VDEVICE(XILINX, PCI_DEVICE_ID_XILINX_B054),
	  .driver_data = (kernel_ulong_t)&xilinx_mdb_match_data },
	{ PCI_VDEVICE(XILINX, PCI_DEVICE_ID_XILINX_B00F),
	  .driver_data = (kernel_ulong_t)&xilinx_cpm6_dma_match_data },
	{ }
};
MODULE_DEVICE_TABLE(pci, dw_edma_pcie_id_table);

static struct pci_driver dw_edma_pcie_driver = {
	.name		= "dw-edma-pcie",
	.id_table	= dw_edma_pcie_id_table,
	.probe		= dw_edma_pcie_probe,
	.remove		= dw_edma_pcie_remove,
	.driver		= {
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};

module_pci_driver(dw_edma_pcie_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Synopsys DesignWare eDMA PCIe driver");
MODULE_AUTHOR("Gustavo Pimentel <gustavo.pimentel@synopsys.com>");
