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
#define DESC_DATA_LEN		GENMASK(13, 0)
#define DESC_BP_EXT		GENMASK(26, 23)
#define DESC_EOP		BIT(28)
#define DESC_SOP		BIT(29)
#define DESC_C			BIT(30)
#define DESC_OWN		BIT(31)
#define DMA_DFT_DESC_NUM	1024

/* RX sideband information from DMA */
struct dma_rx_data {
	unsigned int	data_len;
	unsigned int	chno;
};

struct dw4_desc_hw {
	u32 dw0;
	u32 dw1;
	u32 dw2;
	u32 dw3;
} __packed __aligned(8);

struct dw4_desc_sw {
	struct virt_dma_desc	vd;
	struct ldma_chan	*chan;
	struct dw4_desc_hw	*desc_hw;
};

/**
 * hdma TX need some sideband info to switch in dw0 and dw1
 */
struct chan_cfg {
	u32 desc_dw0;
	u32 desc_dw1;
};

struct hdma_chan {
	struct dw4_desc_sw	*ds;
	struct chan_cfg		cfg;
	struct ldma_chan	*c;
	int			prep_idx; /* desc prep idx*/
	int			comp_idx; /* desc comp idx*/
	int			prep_desc_cnt;
	struct tasklet_struct	task;
};

static int hdma_ctrl_init(struct ldma_dev *d);
static int hdma_port_init(struct ldma_dev *d, struct ldma_port *p);
static int hdma_chan_init(struct ldma_dev *d, struct ldma_chan *c);
static int hdma_irq_init(struct ldma_dev *d, struct platform_device *pdev);
static void hdma_func_init(struct ldma_dev *d, struct dma_device *dma_dev);
static void hdma_free_chan_resources(struct dma_chan *dma_chan);
static void dma_chan_complete(struct tasklet_struct *t);

static inline
struct dw4_desc_sw *to_lgm_dma_dw4_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct dw4_desc_sw, vd);
}

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
	struct hdma_chan *chan;

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

	chan = devm_kzalloc(d->dev, sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;

	c->priv = chan;
	chan->c = c;
	tasklet_setup(&chan->task, dma_chan_complete);

	return 0;
}

static inline void hdma_get_irq_off(int high, u32 *en_off, u32 *cr_off)
{
	if (!high) {
		*cr_off = DMA_IRNCR;
		*en_off = DMA_IRNEN;
	} else {
		*cr_off = DMA_IRNCR1;
		*en_off = DMA_IRNEN1;
	}
}

static inline
void hdma_chan_int_enable(struct ldma_dev *d, struct ldma_chan *c)
{
	unsigned long flags;
	u32 val, en_off, cr_off, cid;

	spin_lock_irqsave(&d->dev_lock, flags);
	/* select DMA channel */
	ldma_update_bits(d, DMA_CS_MASK, c->nr, DMA_CS);
	/* Enable EOP interrupt */
	ldma_update_bits(d, DMA_CI_EOP, DMA_CI_EOP, DMA_CIE);

	val = c->nr >= MAX_LOWER_CHANS ? 1 : 0;
	cid = c->nr >= MAX_LOWER_CHANS ? c->nr - MAX_LOWER_CHANS : c->nr;
	hdma_get_irq_off(val, &en_off, &cr_off);
	ldma_update_bits(d, BIT(cid), BIT(cid), en_off);

	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void dma_chan_complete(struct tasklet_struct *t)
{
	struct hdma_chan *chan = from_tasklet(chan, t, task);
	struct ldma_chan *c = chan->c;
	struct ldma_dev *d = chan_to_ldma_dev(c);

	/* check how many valid descriptor from DMA */
	while (chan->prep_desc_cnt > 0) {
		struct dma_rx_data *data;
		struct dmaengine_desc_callback cb;
		struct dma_async_tx_descriptor *tx;
		struct dw4_desc_sw *desc_sw;
		struct dw4_desc_hw *desc_hw;

		desc_sw = chan->ds + chan->comp_idx;
		desc_hw = desc_sw->desc_hw;
		dma_map_single(d->dev, desc_hw,
			       sizeof(*desc_hw), DMA_FROM_DEVICE);

		/* desc still in processing, stop */
		if (!FIELD_GET(DESC_C, desc_hw->dw3))
			break;

		tx = &desc_sw->vd.tx;
		dmaengine_desc_get_callback(tx, &cb);

		if (is_dma_chan_rx(d)) {
			data = (struct dma_rx_data *)cb.callback_param;
			data->data_len = FIELD_GET(DESC_DATA_LEN, desc_hw->dw3);
			data->chno = c->nr;
		}

		dma_cookie_complete(tx);
		chan->comp_idx = (chan->comp_idx + 1) % c->desc_cnt;
		chan->prep_desc_cnt -= 1;
		dmaengine_desc_callback_invoke(&cb, NULL);
	}

	hdma_chan_int_enable(d, c);
}

static u64 hdma_irq_stat(struct ldma_dev *d, int high)
{
	u32 irnen, irncr, en_off, cr_off, cid;
	unsigned long flags;
	u64 ret;

	spin_lock_irqsave(&d->dev_lock, flags);

	hdma_get_irq_off(high, &en_off, &cr_off);

	irncr = readl(d->base + cr_off);
	irnen = readl(d->base + en_off);

	if (!irncr || !irnen || !(irncr & irnen)) {
		writel(irncr, d->base + cr_off);
		spin_unlock_irqrestore(&d->dev_lock, flags);
		return 0;
	}

	/* disable EOP interrupt for the channel */
	for_each_set_bit(cid, (const unsigned long *)&irncr, d->chan_nrs) {
		/* select DMA channel */
		ldma_update_bits(d, DMA_CS_MASK, cid, DMA_CS);
		/* Clear EOP interrupt status */
		writel(readl(d->base + DMA_CIS), d->base + DMA_CIS);
		/* Disable EOP interrupt */
		writel(0, d->base + DMA_CIE);
	}

	/* ACK interrupt */
	writel(irncr, d->base + cr_off);
	irnen &= ~irncr;
	/* Disable interrupt */
	writel(irnen, d->base + en_off);

	spin_unlock_irqrestore(&d->dev_lock, flags);

	if (high)
		ret = (u64)irncr << 32;
	else
		ret = (u64)irncr;

	return ret;
}

static irqreturn_t hdma_interrupt(int irq, void *dev_id)
{
	struct ldma_dev *d = dev_id;
	struct hdma_chan *chan;
	u32 cid;
	u64 stat;

	stat = hdma_irq_stat(d, 0) | hdma_irq_stat(d, 1);
	if (!stat)
		return IRQ_HANDLED;

	for_each_set_bit(cid, (const unsigned long *)&stat, d->chan_nrs) {
		chan = (struct hdma_chan *)d->chans[cid].priv;
		tasklet_schedule(&chan->task);
	}

	return IRQ_HANDLED;
}

static int hdma_irq_init(struct ldma_dev *d, struct platform_device *pdev)
{
	if (d->flags & DMA_CHAN_HW_DESC)
		return 0;

	d->irq = platform_get_irq(pdev, 0);
	if (d->irq < 0)
		return d->irq;

	return devm_request_irq(d->dev, d->irq, hdma_interrupt, 0,
				DRIVER_NAME, d);
}

/**
 * Allocate DMA descriptor list
 */
static int hdma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct hdma_chan *chan = (struct hdma_chan *)c->priv;
	struct device *dev = c->vchan.chan.device->dev;
	struct dw4_desc_sw *desc_sw;
	struct dw4_desc_hw *desc_hw;
	size_t desc_sz;
	int i;

	/* HW allocate DMA descriptors */
	if (ldma_chan_is_hw_desc(c)) {
		c->flags |= CHAN_IN_USE;
		dev_dbg(dev, "desc in hw\n");
		return 0;
	}

	if (!c->desc_cnt) {
		dev_err(dev, "descriptor count is not set\n");
		return -EINVAL;
	}

	desc_sz = c->desc_cnt * sizeof(*desc_hw);

	c->desc_base = kzalloc(desc_sz, GFP_KERNEL);
	if (!c->desc_base)
		return -ENOMEM;

	c->desc_phys = dma_map_single(dev, c->desc_base,
				      desc_sz, DMA_TO_DEVICE);
	if (dma_mapping_error(dev, c->desc_phys)) {
		dev_err(dev, "dma mapping error for dma desc list\n");
		goto desc_err;
	}

	desc_sz = c->desc_cnt * sizeof(*desc_sw);
	chan->ds = kzalloc(desc_sz, GFP_KERNEL);

	if (!chan->ds)
		goto desc_err;

	desc_hw = (struct dw4_desc_hw *)c->desc_base;
	for (i = 0; i < c->desc_cnt; i++) {
		desc_sw = chan->ds + i;
		desc_sw->chan = c;
		desc_sw->desc_hw = desc_hw + i;
	}

	dev_dbg(dev, "Alloc DMA CH: %u, phy addr: 0x%llx, desc cnt: %u\n",
		c->nr, c->desc_phys, c->desc_cnt);

	ldma_chan_desc_hw_cfg(c, c->desc_phys, c->desc_cnt);
	ldma_chan_on(c);

	return c->desc_cnt;

desc_err:
	kfree(c->desc_base);
	return -EINVAL;
}

static void hdma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct hdma_chan *chan = (struct hdma_chan *)c->priv;
	struct device *dev = c->vchan.chan.device->dev;

	ldma_chan_reset(c);

	/* HW allocate DMA descriptors */
	if (ldma_chan_is_hw_desc(c)) {
		c->flags &= ~CHAN_IN_USE;
		dev_dbg(dev, "%s: desc in hw\n", __func__);
		return;
	}

	vchan_free_chan_resources(&c->vchan);
	kfree(chan->ds);
	kfree(c->desc_base);

	dev_dbg(dev, "Free DMA channel %u\n", c->nr);
}

static int
hdma_slave_config(struct dma_chan *dma_chan, struct dma_slave_config *cfg)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct hdma_chan *chan = (struct hdma_chan *)c->priv;

	if (cfg->peripheral_config)
		memcpy(&chan->cfg, cfg->peripheral_config, sizeof(chan->cfg));

	return 0;
}

static void hdma_desc_set_own(struct dw4_desc_sw *desc_sw)
{
	struct ldma_chan *c = desc_sw->chan;
	struct dw4_desc_hw *desc_hw;
	struct device *dev = c->vchan.chan.device->dev;

	desc_hw = desc_sw->desc_hw;
	desc_hw->dw3 |= DESC_OWN;

	dma_map_single(dev, desc_hw, sizeof(*desc_hw), DMA_TO_DEVICE);
}

static void hdma_execute_pending(struct ldma_chan *c)
{
	struct virt_dma_desc *vd = NULL;
	struct dw4_desc_sw *desc_sw;

	do {
		vd = vchan_next_desc(&c->vchan);
		if (!vd)
			break;
		list_del(&vd->node);
		desc_sw = to_lgm_dma_dw4_desc(vd);
		hdma_desc_set_own(desc_sw);
	} while (vd);
}

static void hdma_issue_pending(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	unsigned long flags;

	if (ldma_chan_is_hw_desc(c)) {
		ldma_chan_on(c);
		return;
	}

	spin_lock_irqsave(&c->vchan.lock, flags);
	if (vchan_issue_pending(&c->vchan))
		hdma_execute_pending(c);
	spin_unlock_irqrestore(&c->vchan.lock, flags);
}

static enum dma_status
hdma_tx_status(struct dma_chan *dma_chan, dma_cookie_t cookie,
	       struct dma_tx_state *txstate)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);

	if (ldma_chan_is_hw_desc(c))
		return DMA_COMPLETE;

	return dma_cookie_status(dma_chan, cookie, txstate);
}

/**
 * initializa the HW DMA descriptor.
 */
static void
hdma_setup_desc(struct ldma_chan *c, struct dw4_desc_hw *desc_hw,
		dma_addr_t paddr, unsigned int len)
{
	struct hdma_chan *chan = (struct hdma_chan *)c->priv;
	u32 dw3 = 0;

	desc_hw->dw0 = chan->cfg.desc_dw0;
	desc_hw->dw1 = chan->cfg.desc_dw1;
	desc_hw->dw2 = lower_32_bits(paddr); /* physicall address */

	dw3 = FIELD_PREP(DESC_DATA_LEN, len);
	dw3 |= FIELD_PREP(DESC_BP_EXT, upper_32_bits(paddr));
	dw3 |= FIELD_PREP(DESC_SOP, 1);
	dw3 |= FIELD_PREP(DESC_EOP, 1);
	desc_hw->dw3 = dw3;
}

static inline
bool hdma_desc_empty(struct hdma_chan *chan, unsigned int desc_cnt)
{
	return (chan->prep_desc_cnt == desc_cnt);
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
	struct hdma_chan *chan = (struct hdma_chan *)c->priv;
	struct device *dev = c->vchan.chan.device->dev;
	struct dw4_desc_hw *desc_hw;
	struct dw4_desc_sw *desc_sw;

	if (!sgl || sglen < 1) {
		dev_err(dev, "%s param error!\n", __func__);
		return NULL;
	}

	if (ldma_chan_is_hw_desc(c))
		return hdma_chan_hw_desc_cfg(dma_chan, sgl->dma_address, sglen);

	if (hdma_desc_empty(chan, c->desc_cnt))
		return NULL;

	desc_sw = chan->ds + chan->prep_idx;
	chan->prep_idx = (chan->prep_idx + 1) % c->desc_cnt;
	desc_hw = desc_sw->desc_hw;
	chan->prep_desc_cnt += 1;

	hdma_setup_desc(c, desc_sw->desc_hw,
			sg_dma_address(sgl), sg_dma_len(sgl));

	return vchan_tx_prep(&c->vchan, &desc_sw->vd,
			     DMA_CTRL_ACK);
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
	dma_dev->device_config = hdma_slave_config;

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
