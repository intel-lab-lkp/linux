// SPDX-License-Identifier: GPL-2.0
/*
 * Lightning Mountain centralized DMA controller driver
 *
 * Copyright (c) 2025 Maxlinear Inc.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/of_dma.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include "../dmaengine.h"
#include "../virt-dma.h"
#include "lgm-dma.h"

/* Descriptor fields */
#define DMA_DFT_DESC_NUM	1024

static int hdma_ctrl_init(struct ldma_dev *d);
static int hdma_port_init(struct ldma_dev *d, struct ldma_port *p);
static int hdma_chan_init(struct ldma_dev *d, struct ldma_chan *c);
static int hdma_irq_init(struct ldma_dev *d, struct platform_device *pdev);
static void hdma_func_init(struct ldma_dev *d, struct dma_device *dma_dev);
static void hdma_free_chan_resources(struct dma_chan *dma_chan);

static inline bool is_dma_chan_tx(struct ldma_dev *d)
{
	return (d->type == DMA_TYPE_TX);
}

static inline bool is_dma_chan_rx(struct ldma_dev *d)
{
	return (d->type == DMA_TYPE_RX);
}

struct ldma_ops ldma_hdma_ops = {
	.dma_ctrl_init = hdma_ctrl_init,
	.dma_port_init = hdma_port_init,
	.dma_chan_init = hdma_chan_init,
	.dma_irq_init  = hdma_irq_init,
	.dma_func_init = hdma_func_init,
};

static int hdma_ctrl_init(struct ldma_dev *d)
{
	return 0;
}

static int hdma_port_init(struct ldma_dev *d, struct ldma_port *p)
{
	p->ldev = d;
	p->rxendi = DMA_DFT_ENDIAN;
	p->txendi = DMA_DFT_ENDIAN;
	p->rxbl = DMA_DFT_BURST;
	p->txbl = DMA_DFT_BURST;
	p->pkt_drop = DMA_PKT_DROP_DIS;

	return 0;
}

static inline void hdma_free_desc_resource(struct virt_dma_desc *vd)
{
}

static int hdma_chan_init(struct ldma_dev *d, struct ldma_chan *c)
{
	c->data_endian = DMA_DFT_ENDIAN;
	c->desc_endian = DMA_DFT_ENDIAN;
	c->data_endian_en = false;
	c->desc_endian_en = false;
	c->desc_rx_np = false;
	c->onoff = DMA_CH_OFF;
	c->rst = DMA_CHAN_RST;
	c->abc_en = true;
	c->hdrm_csum = false;
	c->boff_len = 0;
	c->desc_cnt = DMA_DFT_DESC_NUM;
	c->vchan.desc_free = hdma_free_desc_resource;
	vchan_init(&c->vchan, &d->dma_dev);

	return 0;
}

static int hdma_irq_init(struct ldma_dev *d, struct platform_device *pdev)
{
	return 0;
}

/**
 * Allocate DMA descriptor list
 */
static int hdma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct device *dev = c->vchan.chan.device->dev;

	/* HW allocate DMA descriptors */
	c->flags |= CHAN_IN_USE;
	dev_dbg(dev, "Alloc DMA channel %u\n", c->nr);

	return 0;
}

static void hdma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct device *dev = c->vchan.chan.device->dev;

	ldma_chan_reset(c);

	/* HW allocate DMA descriptors */
	c->flags &= ~CHAN_IN_USE;

	dev_dbg(dev, "Free DMA channel %u\n", c->nr);
}

static void hdma_issue_pending(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);

	ldma_chan_on(c);
}

static enum dma_status
hdma_tx_status(struct dma_chan *dma_chan, dma_cookie_t cookie,
	       struct dma_tx_state *txstate)
{
	return DMA_COMPLETE;
}

/**
 * HW Manipulate DMA descriptors.
 * Only need configure descriptor address and length to DMA.
 */
static struct dma_async_tx_descriptor *
hdma_chan_hw_desc_cfg(struct dma_chan *dma_chan, dma_addr_t desc_base, int desc_num)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct ldma_dev *d = chan_to_ldma_dev(c);

	if (!desc_num) {
		dev_err(d->dev, "Channel %d must allocate descriptor first\n",
			c->nr);
		return NULL;
	}

	if (desc_num > DMA_MAX_DESC_NUM) {
		dev_err(d->dev, "Channel %d descriptor number out of range %d\n",
			c->nr, desc_num);
		return NULL;
	}

	ldma_chan_desc_hw_cfg(c, desc_base, desc_num);

	c->desc_cnt = desc_num;
	c->desc_phys = desc_base;

	return NULL;
}

/**
 *  HDMA driver design to use 1 to 1 SW and HW descriptor mapping
 */
static struct dma_async_tx_descriptor *
hdma_prep_slave_sg(struct dma_chan *dma_chan, struct scatterlist *sgl,
		   unsigned int sglen, enum dma_transfer_direction dir,
		   unsigned long flags, void *context)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct device *dev = c->vchan.chan.device->dev;

	if (!sgl || sglen < 1) {
		dev_err(dev, "%s param error!\n", __func__);
		return NULL;
	}

	return hdma_chan_hw_desc_cfg(dma_chan, sgl->dma_address, sglen);
}

static void hdma_func_init(struct ldma_dev *d, struct dma_device *dma_dev)
{
	dma_dev->device_alloc_chan_resources = hdma_alloc_chan_resources;
	dma_dev->device_free_chan_resources = hdma_free_chan_resources;
	dma_dev->device_terminate_all = ldma_terminate_all;
	dma_dev->device_issue_pending = hdma_issue_pending;
	dma_dev->device_tx_status = hdma_tx_status;
	dma_dev->device_resume = ldma_resume_chan;
	dma_dev->device_pause = ldma_pause_chan;
	dma_dev->device_prep_slave_sg = hdma_prep_slave_sg;

	dma_dev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_8_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_16_BYTES);
	dma_dev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_8_BYTES) |
				BIT(DMA_SLAVE_BUSWIDTH_16_BYTES);
	if (is_dma_chan_tx(d))
		dma_dev->directions = BIT(DMA_MEM_TO_DEV);
	else
		dma_dev->directions = BIT(DMA_DEV_TO_MEM);
	dma_dev->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
}
