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

static int hdma_ctrl_init(struct ldma_dev *d);
static int hdma_port_init(struct ldma_dev *d, struct ldma_port *p);
static int hdma_chan_init(struct ldma_dev *d, struct ldma_chan *c);
static int hdma_irq_init(struct ldma_dev *d, struct platform_device *pdev);
static void hdma_func_init(struct dma_device *dma_dev);
static void hdma_free_chan_resources(struct dma_chan *dma_chan);

struct ldma_ops hdma_ops = {
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

static inline void hdma_free_desc_resource(struct virt_dma_desc *vdesc)
{
}

static int hdma_chan_init(struct ldma_dev *d, struct ldma_chan *c)
{
	c->data_endian = DMA_DFT_ENDIAN;
	c->desc_endian = DMA_DFT_ENDIAN;
	c->data_endian_en = false;
	c->desc_endian_en = false;
	c->desc_rx_np = false;
	c->flags |= DEVICE_ALLOC_DESC;
	c->onoff = DMA_CH_OFF;
	c->rst = DMA_CHAN_RST;
	c->abc_en = true;
	c->hdrm_csum = false;
	c->boff_len = 0;
	c->vchan.desc_free = hdma_free_desc_resource;
	vchan_init(&c->vchan, &d->dma_dev);

	return 0;
}

static int hdma_irq_init(struct ldma_dev *d, struct platform_device *pdev)
{
	return 0;
}

static int hdma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct device *dev = c->vchan.chan.device->dev;

	dev_dbg(dev, "allocate channel resource!\n");

	if (c->flags & DMA_HW_DESC) {
		c->flags |= CHAN_IN_USE;
		dev_dbg(dev, "desc in hw\n");
	}

	return 0;
}

static void hdma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);

	c->flags &= ~CHAN_IN_USE;
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

static void hdma_func_init(struct dma_device *dma_dev)
{
	dma_dev->device_alloc_chan_resources = hdma_alloc_chan_resources;
	dma_dev->device_free_chan_resources = hdma_free_chan_resources;
	dma_dev->device_terminate_all = ldma_terminate_all;
	dma_dev->device_issue_pending = hdma_issue_pending;
	dma_dev->device_tx_status = hdma_tx_status;
	dma_dev->device_resume = ldma_resume_chan;
	dma_dev->device_pause = ldma_pause_chan;
}
