// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2024-2025 Arm Limited
// Arm DMA-350 driver

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define DMANSECCTRL		0x200

#define NSEC_CTRL		0x0c
#define INTREN_ANYCHINTR_EN	BIT(0)

#define DMAINFO			0x0f00

#define DMA_BUILDCFG0		0xb0
#define DMA_CFG_DATA_WIDTH	GENMASK(18, 16)
#define DMA_CFG_ADDR_WIDTH	GENMASK(15, 10)
#define DMA_CFG_NUM_CHANNELS	GENMASK(9, 4)

#define DMA_BUILDCFG1		0xb4
#define DMA_CFG_NUM_TRIGGER_IN	GENMASK(8, 0)

#define IIDR			0xc8
#define IIDR_PRODUCTID		GENMASK(31, 20)
#define IIDR_VARIANT		GENMASK(19, 16)
#define IIDR_REVISION		GENMASK(15, 12)
#define IIDR_IMPLEMENTER	GENMASK(11, 0)

#define PRODUCTID_DMA350	0x3a0
#define IMPLEMENTER_ARM		0x43b

#define DMACH(n)		(0x1000 + 0x0100 * (n))

#define CH_CMD			0x00
#define CH_CMD_RESUME		BIT(5)
#define CH_CMD_PAUSE		BIT(4)
#define CH_CMD_STOP		BIT(3)
#define CH_CMD_DISABLE		BIT(2)
#define CH_CMD_CLEAR		BIT(1)
#define CH_CMD_ENABLE		BIT(0)

#define CH_STATUS		0x04
#define CH_STAT_RESUMEWAIT	BIT(21)
#define CH_STAT_PAUSED		BIT(20)
#define CH_STAT_STOPPED		BIT(19)
#define CH_STAT_DISABLED	BIT(18)
#define CH_STAT_ERR		BIT(17)
#define CH_STAT_DONE		BIT(16)
#define CH_STAT_INTR_ERR	BIT(1)
#define CH_STAT_INTR_DONE	BIT(0)

#define CH_INTREN		0x08
#define CH_INTREN_ERR		BIT(1)
#define CH_INTREN_DONE		BIT(0)

#define CH_CTRL			0x0c
#define CH_CTRL_USEDESTRIGIN	BIT(26)
#define CH_CTRL_USESRCTRIGIN	BIT(25)
#define CH_CTRL_DONETYPE	GENMASK(23, 21)
#define CH_CTRL_REGRELOADTYPE	GENMASK(20, 18)
#define CH_CTRL_XTYPE		GENMASK(11, 9)
#define CH_CTRL_TRANSIZE	GENMASK(2, 0)

#define CH_SRCADDR		0x10
#define CH_SRCADDRHI		0x14
#define CH_DESADDR		0x18
#define CH_DESADDRHI		0x1c
#define CH_XSIZE		0x20
#define CH_XSIZEHI		0x24
#define CH_SRCTRANSCFG		0x28
#define CH_DESTRANSCFG		0x2c
#define CH_CFG_MAXBURSTLEN	GENMASK(19, 16)
#define CH_CFG_PRIVATTR		BIT(11)
#define CH_CFG_SHAREATTR	GENMASK(9, 8)
#define CH_CFG_MEMATTR		GENMASK(7, 0)

#define TRANSCFG_DEVICE					\
	FIELD_PREP(CH_CFG_MAXBURSTLEN, 0xf) |		\
	FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_OSH) |	\
	FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_DEVICE)
#define TRANSCFG_NC					\
	FIELD_PREP(CH_CFG_MAXBURSTLEN, 0xf) |		\
	FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_OSH) |	\
	FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_NC)
#define TRANSCFG_WB					\
	FIELD_PREP(CH_CFG_MAXBURSTLEN, 0xf) |		\
	FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_ISH) |	\
	FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_WB)

#define CH_XADDRINC		0x30
#define CH_XY_DES		GENMASK(31, 16)
#define CH_XY_SRC		GENMASK(15, 0)

#define CH_FILLVAL		0x38
#define CH_SRCTRIGINCFG		0x4c
#define CH_DESTRIGINCFG		0x50
#define CH_TRIGINCFG_BLKSIZE	GENMASK(23, 16)
#define CH_TRIGINCFG_MODE	GENMASK(11, 10)
#define CH_TRIGINCFG_TYPE	GENMASK(9, 8)
#define CH_TRIGINCFG_SEL	GENMASK(7, 0)
#define CH_LINKATTR		0x70
#define CH_LINK_SHAREATTR	GENMASK(9, 8)
#define CH_LINK_MEMATTR		GENMASK(7, 0)

#define CH_AUTOCFG		0x74
#define CH_LINKADDR		0x78
#define CH_LINKADDR_EN		BIT(0)

#define CH_LINKADDRHI		0x7c
#define CH_ERRINFO		0x90
#define CH_ERRINFO_AXIRDPOISERR BIT(18)
#define CH_ERRINFO_AXIWRRESPERR BIT(17)
#define CH_ERRINFO_AXIRDRESPERR BIT(16)

#define CH_BUILDCFG0		0xf8
#define CH_CFG_INC_WIDTH	GENMASK(29, 26)
#define CH_CFG_DATA_WIDTH	GENMASK(24, 22)
#define CH_CFG_DATA_BUF_SIZE	GENMASK(7, 0)

#define CH_BUILDCFG1		0xfc
#define CH_CFG_HAS_CMDLINK	BIT(8)
#define CH_CFG_HAS_TRIGSEL	BIT(7)
#define CH_CFG_HAS_TRIGIN	BIT(5)
#define CH_CFG_HAS_WRAP		BIT(1)


#define LINK_REGCLEAR		BIT(0)
#define LINK_INTREN		BIT(2)
#define LINK_CTRL		BIT(3)
#define LINK_SRCADDR		BIT(4)
#define LINK_SRCADDRHI		BIT(5)
#define LINK_DESADDR		BIT(6)
#define LINK_DESADDRHI		BIT(7)
#define LINK_XSIZE		BIT(8)
#define LINK_XSIZEHI		BIT(9)
#define LINK_SRCTRANSCFG	BIT(10)
#define LINK_DESTRANSCFG	BIT(11)
#define LINK_XADDRINC		BIT(12)
#define LINK_FILLVAL		BIT(14)
#define LINK_SRCTRIGINCFG	BIT(19)
#define LINK_DESTRIGINCFG	BIT(20)
#define LINK_AUTOCFG		BIT(29)
#define LINK_LINKADDR		BIT(30)
#define LINK_LINKADDRHI		BIT(31)

#define D350_SLAVE_CMD_WORDS	14

enum ch_ctrl_donetype {
	CH_CTRL_DONETYPE_NONE = 0,
	CH_CTRL_DONETYPE_CMD = 1,
	CH_CTRL_DONETYPE_CYCLE = 3
};

enum ch_ctrl_xtype {
	CH_CTRL_XTYPE_DISABLE = 0,
	CH_CTRL_XTYPE_CONTINUE = 1,
	CH_CTRL_XTYPE_WRAP = 2,
	CH_CTRL_XTYPE_FILL = 3
};

enum ch_trigincfg_mode {
	CH_TRIGINCFG_MODE_COMMAND = 0,
	CH_TRIGINCFG_MODE_DMA_FC = 2,
	CH_TRIGINCFG_MODE_PERIPH_FC = 3
};

enum ch_trigincfg_type {
	CH_TRIGINCFG_TYPE_SW = 0,
	CH_TRIGINCFG_TYPE_HW = 2,
	CH_TRIGINCFG_TYPE_INTERNAL = 3
};

enum ch_cfg_shareattr {
	SHAREATTR_NSH = 0,
	SHAREATTR_OSH = 2,
	SHAREATTR_ISH = 3
};

enum ch_cfg_memattr {
	MEMATTR_DEVICE = 0x00,
	MEMATTR_NC = 0x44,
	MEMATTR_WB = 0xff
};

struct d350_desc {
	struct virt_dma_desc vd;
	u32 command[16];
	u16 xsize;
	u16 xsizehi;
	u32 *cmds;
	dma_addr_t cmds_dma;	/* DMA API address from dma_alloc_coherent() */
	dma_addr_t cmds_bus;	/* DMA350-visible address for CH_LINKADDR */
	size_t cmds_size;
	u32 *cmd_len;
	size_t ncmds;
	size_t bytes;
	size_t period_len;
	size_t periods;
	size_t period;
	u8 tsz;
	bool cyclic;
};

struct d350_chan_map {
	phys_addr_t cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
	enum dma_data_direction dir;
	bool needs_unmap;
};

struct d350_chan {
	struct virt_dma_chan vc;
	struct d350_desc *desc;
	void __iomem *base;
	int irq;
	enum dma_status status;
	dma_cookie_t cookie;
	u32 residue;
	u32 req;
	u8 tsz;
	bool has_trig;
	bool has_wrap;
	bool coherent;
	struct d350_chan_map map;
	struct dma_slave_config sconfig;
};

struct d350 {
	struct dma_device dma;
	int nchan;
	int nreq;
	struct d350_chan channels[] __counted_by(nchan);
};

static inline struct d350_chan *to_d350_chan(struct dma_chan *chan)
{
	return container_of(chan, struct d350_chan, vc.chan);
}

static inline struct d350_desc *to_d350_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct d350_desc, vd);
}

static void d350_free_cmds(struct device *dev, struct d350_desc *desc)
{
	if (desc->cmds)
		dma_free_coherent(dev, desc->cmds_size, desc->cmds,
				  desc->cmds_dma);
	kfree(desc->cmd_len);
}

static void d350_desc_free(struct virt_dma_desc *vd)
{
	struct d350_desc *desc = to_d350_desc(vd);

	d350_free_cmds(vd->tx.chan->device->dev, desc);
	kfree(desc);
}

static int d350_alloc_cmds(struct dma_chan *dchan, struct d350_desc *desc,
			   size_t ncmds)
{
	size_t cmd_size = D350_SLAVE_CMD_WORDS * sizeof(u32);

	if (check_mul_overflow(ncmds, cmd_size, &desc->cmds_size))
		return -ENOMEM;

	desc->cmds = dma_alloc_coherent(dchan->device->dev, desc->cmds_size,
					&desc->cmds_dma, GFP_NOWAIT);
	if (!desc->cmds)
		return -ENOMEM;

	desc->cmds_bus = desc->cmds_dma;
	desc->ncmds = ncmds;

	return 0;
}

static void d350_unmap_resource(struct d350_chan *dch)
{
	struct device *dev = dch->vc.chan.device->dev;
	struct d350_chan_map *map = &dch->map;

	if (map->dir == DMA_NONE)
		return;

	if (map->needs_unmap)
		dma_unmap_resource(dev, map->dma_addr, map->size, map->dir, 0);

	map->dir = DMA_NONE;
	map->needs_unmap = false;
}

/*
 * Translate a CPU physical/resource address to the address visible to DMA350
 * using the dma-ranges property of its parent bus. This is needed for internal
 * interconnect windows where the DMA master sees slave peripherals at
 * different addresses from the CPU.
 */
static int d350_xlate_parent_dma_range(struct device *dev, phys_addr_t phys,
				       size_t size, dma_addr_t *dma)
{
	struct device_node *parent;
	struct of_range_parser parser;
	struct of_range range;
	int ret = -ENOENT;

	parent = of_get_parent(dev->of_node);
	if (!parent)
		return -ENOENT;

	if (of_pci_dma_range_parser_init(&parser, parent))
		goto out_put;

	for_each_of_range(&parser, &range) {
		u64 offset;

		if (phys < range.cpu_addr)
			continue;

		offset = phys - range.cpu_addr;
		if (offset >= range.size)
			continue;

		if (size > range.size - offset)
			continue;

		*dma = range.bus_addr + offset;
		ret = 0;
		break;
	}

out_put:
	of_node_put(parent);
	return ret;
}

static dma_addr_t d350_map_resource(struct d350_chan *dch,
				    phys_addr_t cpu_addr, size_t size,
				    enum dma_data_direction dir)
{
	struct device *dev = dch->vc.chan.device->dev;
	struct d350_chan_map *map = &dch->map;
	dma_addr_t dma_addr;
	int ret;

	if (map->dir == dir && map->cpu_addr == cpu_addr &&
	    map->size == size)
		return map->dma_addr;

	d350_unmap_resource(dch);

	if (!device_iommu_mapped(dev)) {
		ret = d350_xlate_parent_dma_range(dev, cpu_addr, size,
						  &dma_addr);
		if (!ret)
			goto done;

		if (ret != -ENOENT) {
			dev_err(dev, "translate resource failed ch%u phys=%pa size=%zu\n",
				dch->vc.chan.chan_id, &cpu_addr, size);
			return DMA_MAPPING_ERROR;
		}
	}

	dma_addr = dma_map_resource(dev, cpu_addr, size, dir, 0);
	if (dma_mapping_error(dev, dma_addr)) {
		dev_err(dev, "map slave failed ch%u phys=%pa size=%zu dir=%d\n",
			dch->vc.chan.chan_id, &cpu_addr, size, dir);
		return DMA_MAPPING_ERROR;
	}
	map->needs_unmap = true;

done:
	map->cpu_addr = cpu_addr;
	map->dma_addr = dma_addr;
	map->size = size;
	map->dir = dir;

	return map->dma_addr;
}

static bool d350_buswidth_supported(u32 widths, enum dma_slave_buswidth width)
{
	return width < BITS_PER_TYPE(widths) && (widths & BIT(width));
}

static int d350_check_slave_config(struct dma_device *dma,
				   struct dma_slave_config *config)
{
	u32 maxburst = FIELD_MAX(CH_CFG_MAXBURSTLEN) + 1;
	u32 widths = dma->src_addr_widths | dma->dst_addr_widths;

	if (!d350_buswidth_supported(widths, config->src_addr_width) ||
	    !d350_buswidth_supported(widths, config->dst_addr_width))
		return -EINVAL;

	if (config->src_maxburst > maxburst ||
	    config->dst_maxburst > maxburst)
		return -EINVAL;

	return 0;
}

static int d350_config(struct dma_chan *chan, struct dma_slave_config *config)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct dma_device *dma = chan->device;
	unsigned long flags;
	int ret;

	ret = d350_check_slave_config(dma, config);
	if (ret) {
		dev_err(dma->dev, "invalid slave configuration\n");
		return ret;
	}

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (dch->desc || !list_empty(&dch->vc.desc_allocated) ||
	    !list_empty(&dch->vc.desc_submitted) ||
	    !list_empty(&dch->vc.desc_issued)) {
		spin_unlock_irqrestore(&dch->vc.lock, flags);
		return -EBUSY;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	d350_unmap_resource(dch);
	memcpy(&dch->sconfig, config, sizeof(dch->sconfig));

	return 0;
}

static struct dma_chan *d350_of_xlate(struct of_phandle_args *dma_spec,
				      struct of_dma *ofdma)
{
	struct d350 *dmac = ofdma->of_dma_data;
	struct dma_chan *chan;
	struct d350_chan *dch;
	u32 req;

	if (dma_spec->args_count != 1) {
		dev_err(dmac->dma.dev, "dma phandle must have one argument\n");
		return NULL;
	}

	req = dma_spec->args[0];
	if (req >= dmac->nreq) {
		dev_err(dmac->dma.dev, "invalid DMA request %u, have %d\n",
			req, dmac->nreq);
		return NULL;
	}

	chan = dma_get_any_slave_channel(&dmac->dma);
	if (!chan) {
		dev_err(dmac->dma.dev, "can't get a dma channel\n");
		return NULL;
	}

	dch = to_d350_chan(chan);
	if (!dch->has_trig) {
		dev_err(dmac->dma.dev, "channel %d has no trigger support\n",
			chan->chan_id);
		dma_release_channel(chan);
		return NULL;
	}
	dch->req = req;

	return chan;
}

static u32 d350_device_transcfg(u32 maxburst)
{
	return FIELD_PREP(CH_CFG_MAXBURSTLEN, maxburst - 1) |
	       FIELD_PREP(CH_CFG_SHAREATTR, SHAREATTR_OSH) |
	       FIELD_PREP(CH_CFG_MEMATTR, MEMATTR_DEVICE);
}

static int d350_slave_params(struct d350_chan *dch,
			     enum dma_transfer_direction direction,
			     phys_addr_t *dev_cpu_addr,
			     enum dma_data_direction *dev_dir,
			     enum dma_slave_buswidth *width, u32 *maxburst)
{
	struct dma_slave_config *config = &dch->sconfig;

	if (direction == DMA_MEM_TO_DEV) {
		*dev_cpu_addr = config->dst_addr;
		*dev_dir = DMA_FROM_DEVICE;
		*width = config->dst_addr_width;
		*maxburst = config->dst_maxburst;
	} else if (direction == DMA_DEV_TO_MEM) {
		*dev_cpu_addr = config->src_addr;
		*dev_dir = DMA_TO_DEVICE;
		*width = config->src_addr_width;
		*maxburst = config->src_maxburst;
	} else {
		return -EINVAL;
	}

	return *width && *maxburst ? 0 : -EINVAL;
}

static void d350_fill_slave_cmd(struct d350_chan *dch, struct d350_desc *desc,
				u32 *cmd, dma_addr_t mem, dma_addr_t dev_dma_addr,
				size_t len, dma_addr_t link_addr,
				enum dma_transfer_direction direction,
				enum dma_slave_buswidth width, u32 maxburst,
				enum ch_ctrl_donetype donetype)
{
	bool mem_to_dev = direction == DMA_MEM_TO_DEV;
	u16 xsize, xsizehi;
	u32 devcfg;
	u32 memcfg;
	u32 trigcfg;

	desc->tsz = __ffs(width);
	xsize = lower_16_bits(len >> desc->tsz);
	xsizehi = upper_16_bits(len >> desc->tsz);
	devcfg = d350_device_transcfg(maxburst);
	memcfg = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;

	trigcfg = FIELD_PREP(CH_TRIGINCFG_BLKSIZE,
			     mem_to_dev ? maxburst - 1 : 0) |
		  FIELD_PREP(CH_TRIGINCFG_MODE, CH_TRIGINCFG_MODE_PERIPH_FC) |
		  FIELD_PREP(CH_TRIGINCFG_TYPE, CH_TRIGINCFG_TYPE_HW) |
		  FIELD_PREP(CH_TRIGINCFG_SEL, dch->req);

	cmd[0] = LINK_CTRL | LINK_SRCADDR | LINK_SRCADDRHI | LINK_DESADDR |
		 LINK_DESADDRHI | LINK_XSIZE | LINK_XSIZEHI | LINK_SRCTRANSCFG |
		 LINK_DESTRANSCFG | LINK_XADDRINC | LINK_LINKADDR |
		 LINK_LINKADDRHI |
		 (mem_to_dev ? LINK_DESTRIGINCFG : LINK_SRCTRIGINCFG);
	cmd[1] = (mem_to_dev ? CH_CTRL_USEDESTRIGIN : CH_CTRL_USESRCTRIGIN) |
		 FIELD_PREP(CH_CTRL_TRANSIZE, desc->tsz) |
		 FIELD_PREP(CH_CTRL_XTYPE, CH_CTRL_XTYPE_CONTINUE) |
		 FIELD_PREP(CH_CTRL_DONETYPE, donetype);
	cmd[2] = lower_32_bits(mem_to_dev ? mem : dev_dma_addr);
	cmd[3] = upper_32_bits(mem_to_dev ? mem : dev_dma_addr);
	cmd[4] = lower_32_bits(mem_to_dev ? dev_dma_addr : mem);
	cmd[5] = upper_32_bits(mem_to_dev ? dev_dma_addr : mem);
	cmd[6] = FIELD_PREP(CH_XY_SRC, xsize) |
		 FIELD_PREP(CH_XY_DES, xsize);
	cmd[7] = FIELD_PREP(CH_XY_SRC, xsizehi) |
		 FIELD_PREP(CH_XY_DES, xsizehi);
	cmd[8] = mem_to_dev ? memcfg : devcfg;
	cmd[9] = mem_to_dev ? devcfg : memcfg;
	cmd[10] = mem_to_dev ? FIELD_PREP(CH_XY_SRC, 1) :
				FIELD_PREP(CH_XY_DES, 1);
	cmd[11] = trigcfg;
	cmd[12] = lower_32_bits(link_addr) |
		  (link_addr ? CH_LINKADDR_EN : 0);
	cmd[13] = upper_32_bits(link_addr);
}

static struct dma_async_tx_descriptor *
d350_prep_slave_sg(struct dma_chan *dchan, struct scatterlist *sgl,
		   unsigned int sg_len,
		   enum dma_transfer_direction direction,
		   unsigned long flags, void *context)
{
	struct d350_chan *dch = to_d350_chan(dchan);
	size_t cmd_size = D350_SLAVE_CMD_WORDS * sizeof(u32);
	enum dma_data_direction dev_dir;
	enum dma_slave_buswidth width;
	struct d350_desc *desc;
	phys_addr_t dev_cpu_addr;
	dma_addr_t dev_dma_addr, mem;
	struct scatterlist *sg;
	u32 maxburst;
	size_t len;
	int i;

	if (unlikely(!is_slave_direction(direction) || !sg_len))
		return NULL;

	if (d350_slave_params(dch, direction, &dev_cpu_addr, &dev_dir, &width,
			      &maxburst))
		return NULL;

	dev_dma_addr = d350_map_resource(dch, dev_cpu_addr, width, dev_dir);
	if (dma_mapping_error(dchan->device->dev, dev_dma_addr))
		return NULL;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;

	if (sg_len > 1) {
		if (d350_alloc_cmds(dchan, desc, sg_len))
			goto err_free_desc;

		desc->cmd_len = kcalloc(sg_len, sizeof(*desc->cmd_len),
					GFP_NOWAIT);
		if (!desc->cmd_len)
			goto err_free_cmds;
	}

	for_each_sg(sgl, sg, sg_len, i) {
		enum ch_ctrl_donetype donetype = CH_CTRL_DONETYPE_CMD;
		dma_addr_t link_addr = 0;
		u32 *cmd = desc->command;

		mem = sg_dma_address(sg);
		len = sg_dma_len(sg);
		if (!len || (len >> __ffs(width)) > U32_MAX ||
		    !IS_ALIGNED(len | mem | dev_dma_addr, width))
			goto err_free_cmds;

		if (sg_len > 1) {
			cmd = desc->cmds + i * D350_SLAVE_CMD_WORDS;
			if (i < sg_len - 1) {
				link_addr = desc->cmds_bus + (i + 1) * cmd_size;
				donetype = CH_CTRL_DONETYPE_NONE;
			}
			desc->cmd_len[i] = len;
		}

		if (check_add_overflow(desc->bytes, len, &desc->bytes) ||
		    desc->bytes > U32_MAX)
			goto err_free_cmds;

		d350_fill_slave_cmd(dch, desc, cmd, mem, dev_dma_addr, len,
				    link_addr, direction, width, maxburst,
				    donetype);
	}

	if (sg_len > 1)
		memcpy(desc->command, desc->cmds, cmd_size);

	return vchan_tx_prep(&dch->vc, &desc->vd, flags);

err_free_cmds:
	d350_free_cmds(dchan->device->dev, desc);
err_free_desc:
	kfree(desc);

	return NULL;
}

static struct dma_async_tx_descriptor *
d350_prep_dma_cyclic(struct dma_chan *dchan, dma_addr_t buf_addr,
		     size_t buf_len, size_t period_len,
		     enum dma_transfer_direction direction, unsigned long flags)
{
	struct d350_chan *dch = to_d350_chan(dchan);
	struct d350_desc *desc;
	phys_addr_t dev_cpu_addr;
	dma_addr_t dev_dma_addr;
	enum dma_data_direction dev_dir;
	enum dma_slave_buswidth width;
	size_t period, cmd_size;
	u32 maxburst;
	int ret;

	if (!buf_len || !period_len || buf_len % period_len ||
	    buf_len > U32_MAX || !is_slave_direction(direction))
		return NULL;

	ret = d350_slave_params(dch, direction, &dev_cpu_addr, &dev_dir, &width,
				&maxburst);
	if (ret)
		return NULL;

	dev_dma_addr = d350_map_resource(dch, dev_cpu_addr, width, dev_dir);
	if (dma_mapping_error(dchan->device->dev, dev_dma_addr))
		return NULL;

	if (!IS_ALIGNED(buf_addr | dev_dma_addr | period_len, width))
		return NULL;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->bytes = buf_len;
	desc->period_len = period_len;
	desc->periods = buf_len / period_len;
	desc->cyclic = true;

	if (d350_alloc_cmds(dchan, desc, desc->periods)) {
		kfree(desc);
		return NULL;
	}

	cmd_size = D350_SLAVE_CMD_WORDS * sizeof(u32);

	for (period = 0; period < desc->periods; period++) {
		u32 *cmd = desc->cmds + period * D350_SLAVE_CMD_WORDS;
		dma_addr_t mem = buf_addr + period * period_len;
		dma_addr_t next = desc->cmds_bus +
				  ((period + 1) % desc->periods) * cmd_size;

		d350_fill_slave_cmd(dch, desc, cmd, mem, dev_dma_addr,
				    period_len, next, direction, width, maxburst,
				    CH_CTRL_DONETYPE_CMD);
	}
	memcpy(desc->command, desc->cmds, cmd_size);

	return vchan_tx_prep(&dch->vc, &desc->vd, flags);
}

static struct dma_async_tx_descriptor *d350_prep_memcpy(struct dma_chan *chan,
		dma_addr_t dest, dma_addr_t src, size_t len, unsigned long flags)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct d350_desc *desc;
	u32 *cmd;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->bytes = len;
	desc->tsz = __ffs(len | dest | src | (1 << dch->tsz));
	desc->xsize = lower_16_bits(len >> desc->tsz);
	desc->xsizehi = upper_16_bits(len >> desc->tsz);

	cmd = desc->command;
	cmd[0] = LINK_CTRL | LINK_SRCADDR | LINK_SRCADDRHI | LINK_DESADDR |
		 LINK_DESADDRHI | LINK_XSIZE | LINK_XSIZEHI | LINK_SRCTRANSCFG |
		 LINK_DESTRANSCFG | LINK_XADDRINC | LINK_LINKADDR;

	cmd[1] = FIELD_PREP(CH_CTRL_TRANSIZE, desc->tsz) |
		 FIELD_PREP(CH_CTRL_XTYPE, CH_CTRL_XTYPE_CONTINUE) |
		 FIELD_PREP(CH_CTRL_DONETYPE, CH_CTRL_DONETYPE_CMD);

	cmd[2] = lower_32_bits(src);
	cmd[3] = upper_32_bits(src);
	cmd[4] = lower_32_bits(dest);
	cmd[5] = upper_32_bits(dest);
	cmd[6] = FIELD_PREP(CH_XY_SRC, desc->xsize) | FIELD_PREP(CH_XY_DES, desc->xsize);
	cmd[7] = FIELD_PREP(CH_XY_SRC, desc->xsizehi) | FIELD_PREP(CH_XY_DES, desc->xsizehi);
	cmd[8] = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;
	cmd[9] = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;
	cmd[10] = FIELD_PREP(CH_XY_SRC, 1) | FIELD_PREP(CH_XY_DES, 1);
	cmd[11] = 0;

	return vchan_tx_prep(&dch->vc, &desc->vd, flags);
}

static struct dma_async_tx_descriptor *d350_prep_memset(struct dma_chan *chan,
		dma_addr_t dest, int value, size_t len, unsigned long flags)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct d350_desc *desc;
	u32 *cmd;

	desc = kzalloc_obj(*desc, GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->bytes = len;
	desc->tsz = __ffs(len | dest | (1 << dch->tsz));
	desc->xsize = lower_16_bits(len >> desc->tsz);
	desc->xsizehi = upper_16_bits(len >> desc->tsz);

	cmd = desc->command;
	cmd[0] = LINK_CTRL | LINK_DESADDR | LINK_DESADDRHI |
		 LINK_XSIZE | LINK_XSIZEHI | LINK_DESTRANSCFG |
		 LINK_XADDRINC | LINK_FILLVAL | LINK_LINKADDR;

	cmd[1] = FIELD_PREP(CH_CTRL_TRANSIZE, desc->tsz) |
		 FIELD_PREP(CH_CTRL_XTYPE, CH_CTRL_XTYPE_FILL) |
		 FIELD_PREP(CH_CTRL_DONETYPE, CH_CTRL_DONETYPE_CMD);

	cmd[2] = lower_32_bits(dest);
	cmd[3] = upper_32_bits(dest);
	cmd[4] = FIELD_PREP(CH_XY_DES, desc->xsize);
	cmd[5] = FIELD_PREP(CH_XY_DES, desc->xsizehi);
	cmd[6] = dch->coherent ? TRANSCFG_WB : TRANSCFG_NC;
	cmd[7] = FIELD_PREP(CH_XY_DES, 1);
	cmd[8] = (u8)value * 0x01010101;
	cmd[9] = 0;

	return vchan_tx_prep(&dch->vc, &desc->vd, flags);
}

static int d350_pause(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (dch->status == DMA_IN_PROGRESS) {
		writel_relaxed(CH_CMD_PAUSE, dch->base + CH_CMD);
		dch->status = DMA_PAUSED;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	return 0;
}

static int d350_resume(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (dch->status == DMA_PAUSED) {
		writel_relaxed(CH_CMD_RESUME, dch->base + CH_CMD);
		dch->status = DMA_IN_PROGRESS;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	return 0;
}

static u32 d350_get_residue(struct d350_chan *dch)
{
	u32 res, xsize, xsizehi, hi_new;
	int retries = 3; /* 1st time unlucky, 2nd improbable, 3rd just broken */

	hi_new = readl_relaxed(dch->base + CH_XSIZEHI);
	do {
		xsizehi = hi_new;
		xsize = readl_relaxed(dch->base + CH_XSIZE);
		hi_new = readl_relaxed(dch->base + CH_XSIZEHI);
	} while (xsizehi != hi_new && --retries);

	res = FIELD_GET(CH_XY_DES, xsize);
	res |= FIELD_GET(CH_XY_DES, xsizehi) << 16;

	return res << dch->desc->tsz;
}

static u32 d350_get_sg_residue(struct d350_chan *dch)
{
	struct d350_desc *desc = dch->desc;
	size_t cmd_size = D350_SLAVE_CMD_WORDS * sizeof(u32);
	size_t cmd = 0, i;
	u32 residue;
	u64 next_cmd;

	if (!desc->cmd_len)
		return d350_get_residue(dch);

	/*
	 * CH_LINKADDR points at the next command. Match it against the command
	 * array to find the command currently executing, then add every later
	 * command which has not started yet.
	 */
	next_cmd = readl_relaxed(dch->base + CH_LINKADDR) & ~CH_LINKADDR_EN;
	next_cmd |= (u64)readl_relaxed(dch->base + CH_LINKADDRHI) << 32;

	if (!next_cmd) {
		cmd = desc->ncmds - 1;
	} else {
		for (i = 1; i < desc->ncmds; i++) {
			if (next_cmd == desc->cmds_bus + i * cmd_size) {
				cmd = i - 1;
				break;
			}
		}
		if (i == desc->ncmds)
			return dch->residue;
	}

	residue = d350_get_residue(dch);
	for (i = cmd + 1; i < desc->ncmds; i++)
		residue += desc->cmd_len[i];

	return residue;
}

static int d350_terminate_all(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;
	LIST_HEAD(list);

	spin_lock_irqsave(&dch->vc.lock, flags);
	writel_relaxed(CH_CMD_STOP, dch->base + CH_CMD);
	if (dch->desc) {
		if (dch->status != DMA_ERROR)
			vchan_terminate_vdesc(&dch->desc->vd);
		dch->desc = NULL;
		dch->status = DMA_COMPLETE;
	}
	vchan_get_all_descriptors(&dch->vc, &list);
	list_splice_tail(&list, &dch->vc.desc_terminated);
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	return 0;
}

static void d350_synchronize(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);

	vchan_synchronize(&dch->vc);
}

static u32 d350_desc_bytes(struct d350_desc *desc)
{
	return desc->bytes;
}

static u32 d350_get_cyclic_residue(struct d350_desc *desc)
{
	return desc->bytes - desc->period * desc->period_len;
}

static u32 d350_get_active_residue(struct d350_chan *dch)
{
	if (dch->desc->cyclic)
		return d350_get_cyclic_residue(dch->desc);

	return d350_get_sg_residue(dch);
}

static enum dma_status d350_tx_status(struct dma_chan *chan, dma_cookie_t cookie,
				      struct dma_tx_state *state)
{
	struct d350_chan *dch = to_d350_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status status;
	unsigned long flags;
	u32 residue = 0;

	status = dma_cookie_status(chan, cookie, state);

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (cookie == dch->cookie) {
		status = dch->status;
		if (status == DMA_IN_PROGRESS || status == DMA_PAUSED)
			dch->residue = d350_get_active_residue(dch);
		residue = dch->residue;
	} else if ((vd = vchan_find_desc(&dch->vc, cookie))) {
		residue = d350_desc_bytes(to_d350_desc(vd));
	} else if (status == DMA_IN_PROGRESS) {
		/* Somebody else terminated it? */
		status = DMA_ERROR;
	}
	spin_unlock_irqrestore(&dch->vc.lock, flags);

	dma_set_residue(state, residue);
	return status;
}

static void d350_start_next(struct d350_chan *dch)
{
	u32 hdr, *reg;

	dch->desc = to_d350_desc(vchan_next_desc(&dch->vc));
	if (!dch->desc)
		return;

	list_del(&dch->desc->vd.node);
	dch->status = DMA_IN_PROGRESS;
	dch->cookie = dch->desc->vd.tx.cookie;
	dch->residue = d350_desc_bytes(dch->desc);

	hdr = dch->desc->command[0];
	reg = &dch->desc->command[1];

	if (hdr & LINK_INTREN)
		writel_relaxed(*reg++, dch->base + CH_INTREN);
	if (hdr & LINK_CTRL)
		writel_relaxed(*reg++, dch->base + CH_CTRL);
	if (hdr & LINK_SRCADDR)
		writel_relaxed(*reg++, dch->base + CH_SRCADDR);
	if (hdr & LINK_SRCADDRHI)
		writel_relaxed(*reg++, dch->base + CH_SRCADDRHI);
	if (hdr & LINK_DESADDR)
		writel_relaxed(*reg++, dch->base + CH_DESADDR);
	if (hdr & LINK_DESADDRHI)
		writel_relaxed(*reg++, dch->base + CH_DESADDRHI);
	if (hdr & LINK_XSIZE)
		writel_relaxed(*reg++, dch->base + CH_XSIZE);
	if (hdr & LINK_XSIZEHI)
		writel_relaxed(*reg++, dch->base + CH_XSIZEHI);
	if (hdr & LINK_SRCTRANSCFG)
		writel_relaxed(*reg++, dch->base + CH_SRCTRANSCFG);
	if (hdr & LINK_DESTRANSCFG)
		writel_relaxed(*reg++, dch->base + CH_DESTRANSCFG);
	if (hdr & LINK_XADDRINC)
		writel_relaxed(*reg++, dch->base + CH_XADDRINC);
	if (hdr & LINK_FILLVAL)
		writel_relaxed(*reg++, dch->base + CH_FILLVAL);
	if (hdr & LINK_SRCTRIGINCFG)
		writel_relaxed(*reg++, dch->base + CH_SRCTRIGINCFG);
	if (hdr & LINK_DESTRIGINCFG)
		writel_relaxed(*reg++, dch->base + CH_DESTRIGINCFG);
	if (hdr & LINK_AUTOCFG)
		writel_relaxed(*reg++, dch->base + CH_AUTOCFG);
	if (hdr & LINK_LINKADDR)
		writel_relaxed(*reg++, dch->base + CH_LINKADDR);
	if (hdr & LINK_LINKADDRHI)
		writel_relaxed(*reg++, dch->base + CH_LINKADDRHI);

	writel(CH_CMD_ENABLE, dch->base + CH_CMD);
}

static void d350_issue_pending(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&dch->vc.lock, flags);
	if (vchan_issue_pending(&dch->vc) && !dch->desc)
		d350_start_next(dch);
	spin_unlock_irqrestore(&dch->vc.lock, flags);
}

static irqreturn_t d350_irq(int irq, void *data)
{
	struct d350_chan *dch = data;
	struct virt_dma_desc *vd;
	struct d350_desc *desc;
	u32 residue = 0;
	u32 ch_status;
	u32 irq_status;
	u32 errinfo = 0;

	ch_status = readl(dch->base + CH_STATUS);
	irq_status = ch_status & (CH_STAT_INTR_DONE | CH_STAT_INTR_ERR);
	if (!irq_status)
		return IRQ_NONE;

	if (irq_status & CH_STAT_INTR_ERR)
		errinfo = readl_relaxed(dch->base + CH_ERRINFO);

	writel_relaxed(ch_status, dch->base + CH_STATUS);

	spin_lock(&dch->vc.lock);
	desc = dch->desc;
	if (!desc) {
		spin_unlock(&dch->vc.lock);
		return IRQ_HANDLED;
	}

	vd = &desc->vd;
	if (irq_status & CH_STAT_INTR_ERR) {
		if (errinfo & (CH_ERRINFO_AXIRDPOISERR | CH_ERRINFO_AXIRDRESPERR))
			vd->tx_result.result = DMA_TRANS_READ_FAILED;
		else if (errinfo & CH_ERRINFO_AXIWRRESPERR)
			vd->tx_result.result = DMA_TRANS_WRITE_FAILED;
		else
			vd->tx_result.result = DMA_TRANS_ABORTED;

		residue = d350_get_active_residue(dch);
		vd->tx_result.residue = residue;
		dch->status = DMA_ERROR;
		dch->residue = residue;
		dch->desc = NULL;
		if (desc->cyclic)
			vchan_terminate_vdesc(vd);
		else
			vchan_cookie_complete(vd);
	} else {
		if (desc->cyclic) {
			desc->period = (desc->period + 1) % desc->periods;
			dch->residue = d350_get_cyclic_residue(desc);
			vchan_cyclic_callback(vd);
		} else {
			dch->status = DMA_COMPLETE;
			dch->residue = 0;
			dch->desc = NULL;
			vchan_cookie_complete(vd);
			d350_start_next(dch);
		}
	}
	spin_unlock(&dch->vc.lock);

	return IRQ_HANDLED;
}

static int d350_alloc_chan_resources(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);
	int ret = request_irq(dch->irq, d350_irq, IRQF_SHARED,
			      dev_name(&dch->vc.chan.dev->device), dch);
	if (!ret)
		writel_relaxed(CH_INTREN_DONE | CH_INTREN_ERR, dch->base + CH_INTREN);

	return ret;
}

static void d350_free_chan_resources(struct dma_chan *chan)
{
	struct d350_chan *dch = to_d350_chan(chan);

	writel_relaxed(0, dch->base + CH_INTREN);
	free_irq(dch->irq, dch);
	d350_unmap_resource(dch);
	vchan_free_chan_resources(&dch->vc);
}

static int d350_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct d350 *dmac;
	void __iomem *base;
	u32 reg;
	int ret, nchan, dw, aw, r, p;
	bool coherent, memset;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	reg = readl_relaxed(base + DMAINFO + IIDR);
	r = FIELD_GET(IIDR_VARIANT, reg);
	p = FIELD_GET(IIDR_REVISION, reg);
	if (FIELD_GET(IIDR_IMPLEMENTER, reg) != IMPLEMENTER_ARM ||
	    FIELD_GET(IIDR_PRODUCTID, reg) != PRODUCTID_DMA350)
		return dev_err_probe(dev, -ENODEV, "Not a DMA-350!");

	reg = readl_relaxed(base + DMAINFO + DMA_BUILDCFG0);
	nchan = FIELD_GET(DMA_CFG_NUM_CHANNELS, reg) + 1;
	dw = 1 << FIELD_GET(DMA_CFG_DATA_WIDTH, reg);
	aw = FIELD_GET(DMA_CFG_ADDR_WIDTH, reg) + 1;

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(aw));
	coherent = device_get_dma_attr(dev) == DEV_DMA_COHERENT;

	dmac = devm_kzalloc(dev, struct_size(dmac, channels, nchan), GFP_KERNEL);
	if (!dmac)
		return -ENOMEM;

	dmac->nchan = nchan;

	reg = readl_relaxed(base + DMAINFO + DMA_BUILDCFG1);
	dmac->nreq = FIELD_GET(DMA_CFG_NUM_TRIGGER_IN, reg);

	dev_dbg(dev, "DMA-350 r%dp%d with %d channels, %d requests\n", r, p, dmac->nchan, dmac->nreq);

	dmac->dma.dev = dev;
	dmac->dma.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_UNDEFINED);
	dmac->dma.dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_UNDEFINED);
	for (int i = min(dw, 16); i > 0; i /= 2) {
		dmac->dma.src_addr_widths |= BIT(i);
		dmac->dma.dst_addr_widths |= BIT(i);
	}
	dmac->dma.directions = BIT(DMA_MEM_TO_MEM) |
			BIT(DMA_MEM_TO_DEV) |
			BIT(DMA_DEV_TO_MEM);
	dmac->dma.descriptor_reuse = true;
	dmac->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;
	dmac->dma.device_alloc_chan_resources = d350_alloc_chan_resources;
	dmac->dma.device_free_chan_resources = d350_free_chan_resources;
	dma_cap_set(DMA_MEMCPY, dmac->dma.cap_mask);
	dma_cap_set(DMA_SLAVE, dmac->dma.cap_mask);
	dma_cap_set(DMA_CYCLIC, dmac->dma.cap_mask);
	dmac->dma.device_prep_dma_memcpy = d350_prep_memcpy;
	dmac->dma.device_prep_slave_sg = d350_prep_slave_sg;
	dmac->dma.device_prep_dma_cyclic = d350_prep_dma_cyclic;
	dmac->dma.device_pause = d350_pause;
	dmac->dma.device_resume = d350_resume;
	dmac->dma.device_terminate_all = d350_terminate_all;
	dmac->dma.device_synchronize = d350_synchronize;
	dmac->dma.device_tx_status = d350_tx_status;
	dmac->dma.device_issue_pending = d350_issue_pending;
	dmac->dma.device_config = d350_config;
	INIT_LIST_HEAD(&dmac->dma.channels);

	reg = readl_relaxed(base + DMANSECCTRL + NSEC_CTRL);
	writel_relaxed(reg | INTREN_ANYCHINTR_EN,
		       base + DMANSECCTRL + NSEC_CTRL);

	/* Would be nice to have per-channel caps for this... */
	memset = true;
	for (int i = 0; i < nchan; i++) {
		struct d350_chan *dch = &dmac->channels[i];

		dch->base = base + DMACH(i);
		writel_relaxed(CH_CMD_CLEAR, dch->base + CH_CMD);

		reg = readl_relaxed(dch->base + CH_BUILDCFG1);
		if (!(FIELD_GET(CH_CFG_HAS_CMDLINK, reg))) {
			dev_warn(dev, "No command link support on channel %d\n", i);
			continue;
		}
		dch->irq = platform_get_irq(pdev, i);
		if (dch->irq < 0)
			return dev_err_probe(dev, dch->irq,
					     "Failed to get IRQ for channel %d\n", i);

		dch->has_wrap = FIELD_GET(CH_CFG_HAS_WRAP, reg);
		dch->has_trig = FIELD_GET(CH_CFG_HAS_TRIGIN, reg) &
				FIELD_GET(CH_CFG_HAS_TRIGSEL, reg);

		/* Fill is a special case of Wrap */
		memset &= dch->has_wrap;

		reg = readl_relaxed(dch->base + CH_BUILDCFG0);
		dch->tsz = FIELD_GET(CH_CFG_DATA_WIDTH, reg);

		reg = FIELD_PREP(CH_LINK_SHAREATTR, coherent ? SHAREATTR_ISH : SHAREATTR_OSH);
		reg |= FIELD_PREP(CH_LINK_MEMATTR, coherent ? MEMATTR_WB : MEMATTR_NC);
		writel_relaxed(reg, dch->base + CH_LINKATTR);

		dch->coherent = coherent;
		dch->map.dir = DMA_NONE;
		dch->vc.desc_free = d350_desc_free;
		vchan_init(&dch->vc, &dmac->dma);
	}

	if (memset) {
		dma_cap_set(DMA_MEMSET, dmac->dma.cap_mask);
		dmac->dma.device_prep_dma_memset = d350_prep_memset;
	}

	platform_set_drvdata(pdev, dmac);

	ret = dma_async_device_register(&dmac->dma);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register DMA device\n");

	ret = of_dma_controller_register(dev->of_node, d350_of_xlate, dmac);
	if (ret) {
		dma_async_device_unregister(&dmac->dma);
		return dev_err_probe(dev, ret,
				     "Failed to register OF DMA controller\n");
	}

	return 0;
}

static void d350_remove(struct platform_device *pdev)
{
	struct d350 *dmac = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&dmac->dma);
}

static const struct of_device_id d350_of_match[] __maybe_unused = {
	{ .compatible = "arm,dma-350" },
	{}
};
MODULE_DEVICE_TABLE(of, d350_of_match);

static struct platform_driver d350_driver = {
	.driver = {
		.name = "arm-dma350",
		.of_match_table = of_match_ptr(d350_of_match),
	},
	.probe = d350_probe,
	.remove = d350_remove,
};
module_platform_driver(d350_driver);

MODULE_AUTHOR("Robin Murphy <robin.murphy@arm.com>");
MODULE_DESCRIPTION("Arm DMA-350 driver");
MODULE_LICENSE("GPL v2");
