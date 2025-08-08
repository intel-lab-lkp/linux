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

#define DESC_DATA_LEN		GENMASK(15, 0)
#define DMA_DFT_DESC_NUM	1

struct dw2_desc {
	u32 field;
	u32 addr;
} __packed __aligned(8);

struct dw2_desc_sw {
	struct virt_dma_desc	vdesc;
	struct ldma_chan	*chan;
	dma_addr_t		desc_phys;
	size_t			desc_cnt;
	size_t			size;
	struct dw2_desc		*desc_hw;
};

struct cdma_chan {
	struct dma_pool		*desc_pool; /* Descriptors pool */
	u32			desc_cnt; /* Descriptor length */
	struct dw2_desc_sw	*ds;
	struct work_struct	work;
	struct dma_slave_config config;
};

static int cdma_ctrl_init(struct ldma_dev *d);
static int cdma_port_init(struct ldma_dev *d, struct ldma_port *p);
static int cdma_chan_init(struct ldma_dev *d, struct ldma_chan *c);
static int cdma_irq_init(struct ldma_dev *d, struct platform_device *pdev);
static void cdma_func_init(struct ldma_dev *d, struct dma_device *dma_dev);
static irqreturn_t cdma_interrupt(int irq, void *dev_id);

static struct workqueue_struct	*wq_work;

struct ldma_ops ldma_cdma_ops = {
	.dma_ctrl_init = cdma_ctrl_init,
	.dma_port_init = cdma_port_init,
	.dma_chan_init = cdma_chan_init,
	.dma_irq_init  = cdma_irq_init,
	.dma_func_init = cdma_func_init,
};

static inline struct dw2_desc_sw *to_lgm_cdma_desc(struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct dw2_desc_sw, vdesc);
}

static void cdma_free_desc_resource(struct virt_dma_desc *vdesc)
{
	struct dw2_desc_sw *ds = to_lgm_cdma_desc(vdesc);
	struct ldma_chan *c = ds->chan;
	struct cdma_chan *chan = c->priv;

	dma_pool_free(chan->desc_pool, ds->desc_hw, ds->desc_phys);
	kfree(ds);
	chan->ds = NULL;
}

static void cdma_work(struct work_struct *work)
{
	struct ldma_chan *c;
	struct cdma_chan *chan;
	struct dma_async_tx_descriptor *tx;
	struct virt_dma_chan *vc;
	struct dmaengine_desc_callback cb;
	struct virt_dma_desc *vd, *_vd;
	unsigned long flags;
	LIST_HEAD(head);

	chan = container_of(work, struct cdma_chan, work);
	if (!chan->ds)
		return;
	c = chan->ds[0].chan;
	tx = &chan->ds->vdesc.tx;
	vc = &c->vchan;

	spin_lock_irqsave(&c->vchan.lock, flags);
	list_splice_tail_init(&vc->desc_completed, &head);
	spin_unlock_irqrestore(&c->vchan.lock, flags);
	dmaengine_desc_get_callback(tx, &cb);
	dma_cookie_complete(tx);
	dmaengine_desc_callback_invoke(&cb, NULL);

	list_for_each_entry_safe(vd, _vd, &head, node) {
		dmaengine_desc_get_callback(tx, &cb);
		dma_cookie_complete(tx);
		list_del(&vd->node);
		dmaengine_desc_callback_invoke(&cb, NULL);

		vchan_vdesc_fini(vd);
	}
}

static int cdma_ctrl_init(struct ldma_dev *d)
{
	wq_work = alloc_ordered_workqueue("dma_wq", WQ_MEM_RECLAIM | WQ_HIGHPRI);
	if (!wq_work)
		return -ENOMEM;

	return 0;
}

static int cdma_port_init(struct ldma_dev *d, struct ldma_port *p)
{
	p->ldev = d;
	p->rxendi = DMA_DFT_ENDIAN;
	p->txendi = DMA_DFT_ENDIAN;
	p->rxbl = DMA_DFT_BURST_V22;
	p->txbl = DMA_DFT_BURST_V22;

	return 0;
}

static int cdma_chan_init(struct ldma_dev *d, struct ldma_chan *c)
{
	struct cdma_chan *chan;

	c->rst = DMA_CHAN_RST;
	c->desc_cnt = DMA_DFT_DESC_NUM;
	snprintf(c->name, sizeof(c->name), "chan%d", c->nr);
	c->vchan.desc_free = cdma_free_desc_resource;
	vchan_init(&c->vchan, &d->dma_dev);

	chan = devm_kzalloc(d->dev, sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;

	INIT_WORK(&chan->work, cdma_work);
	c->priv = chan;

	return 0;
}

static int cdma_irq_init(struct ldma_dev *d, struct platform_device *pdev)
{
	d->irq = platform_get_irq(pdev, 0);
	if (d->irq < 0)
		return d->irq;

	return devm_request_irq(d->dev, d->irq, cdma_interrupt, 0,
				DRIVER_NAME, d);
}

static void cdma_chan_irq(int irq, void *data)
{
	struct ldma_chan *c = data;
	struct ldma_dev *d = chan_to_ldma_dev(c);
	struct cdma_chan *chan;
	u32 stat;

	/* Disable channel interrupts  */
	writel(c->nr, d->base + DMA_CS);
	stat = readl(d->base + DMA_CIS);
	if (!stat)
		return;

	writel(readl(d->base + DMA_CIE) & ~DMA_CI_ALL, d->base + DMA_CIE);
	writel(stat, d->base + DMA_CIS);
	chan = (struct cdma_chan *)c->priv;
	queue_work(wq_work, &chan->work);
}

static irqreturn_t cdma_interrupt(int irq, void *dev_id)
{
	struct ldma_dev *d = dev_id;
	struct ldma_chan *c;
	unsigned long irncr;
	u32 cid;

	irncr = readl(d->base + DMA_IRNCR);
	if (!irncr) {
		dev_err(d->dev, "dummy interrupt\n");
		return IRQ_NONE;
	}

	for_each_set_bit(cid, &irncr, d->chan_nrs) {
		/* Mask */
		writel(readl(d->base + DMA_IRNEN) & ~BIT(cid), d->base + DMA_IRNEN);
		/* Ack */
		writel(readl(d->base + DMA_IRNCR) | BIT(cid), d->base + DMA_IRNCR);

		c = &d->chans[cid];
		cdma_chan_irq(irq, c);
	}

	return IRQ_HANDLED;
}

static int cdma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct ldma_dev *d = chan_to_ldma_dev(c);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;
	struct device *dev = d->dev;
	size_t desc_sz;

	if (chan->desc_pool)
		return c->desc_cnt;

	desc_sz = c->desc_cnt * sizeof(struct dw2_desc);
	chan->desc_pool = dma_pool_create(c->name, dev, desc_sz,
					  __alignof__(struct dw2_desc), 0);

	if (!chan->desc_pool) {
		dev_err(dev, "unable to allocate descriptor pool\n");
		return -ENOMEM;
	}
	chan->desc_cnt = c->desc_cnt;

	return c->desc_cnt;
}

static void cdma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;

	dma_pool_destroy(chan->desc_pool);
	chan->desc_pool = NULL;
	vchan_free_chan_resources(to_virt_chan(dma_chan));
	ldma_chan_reset(c);
}

static void cdma_synchronize(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;

	/*
	 * clear any pending work if any. In that
	 * case the resource needs to be free here.
	 */
	cancel_work_sync(&chan->work);
	vchan_synchronize(&c->vchan);
	if (chan->ds)
		cdma_free_desc_resource(&chan->ds->vdesc);
}

static int
cdma_slave_config(struct dma_chan *dma_chan, struct dma_slave_config *cfg)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;

	memcpy(&chan->config, cfg, sizeof(chan->config));

	return 0;
}

static void cdma_chan_irq_en(struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	unsigned long flags;

	spin_lock_irqsave(&d->dev_lock, flags);
	writel(c->nr, d->base + DMA_CS);
	writel(DMA_CI_EOP, d->base + DMA_CIE);
	writel(BIT(c->nr), d->base + DMA_IRNEN);
	spin_unlock_irqrestore(&d->dev_lock, flags);
}

static void cdma_issue_pending(struct dma_chan *dma_chan)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;
	unsigned long flags;

	spin_lock_irqsave(&c->vchan.lock, flags);
	if (vchan_issue_pending(&c->vchan)) {
		struct virt_dma_desc *vdesc;

		/* Get the next descriptor */
		vdesc = vchan_next_desc(&c->vchan);
		if (!vdesc) {
			chan->ds = NULL;
			spin_unlock_irqrestore(&c->vchan.lock, flags);
			return;
		}
		list_del(&vdesc->node);
		chan->ds = to_lgm_cdma_desc(vdesc);
		ldma_chan_desc_hw_cfg(c, chan->ds->desc_phys,
				      chan->ds->desc_cnt);
		cdma_chan_irq_en(c);
	}
	spin_unlock_irqrestore(&c->vchan.lock, flags);

	ldma_chan_on(c);
}

static enum dma_status
cdma_tx_status(struct dma_chan *dma_chan, dma_cookie_t cookie,
	       struct dma_tx_state *txstate)
{
	enum dma_status status = DMA_COMPLETE;

	status = dma_cookie_status(dma_chan, cookie, txstate);

	return status;
}

static struct dw2_desc_sw *
cdma_alloc_desc_resource(int num, struct ldma_chan *c)
{
	struct ldma_dev *d = chan_to_ldma_dev(c);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;
	struct dw2_desc_sw *ds;

	if (num > c->desc_cnt) {
		dev_err(d->dev, "sg num %d exceed max %d\n", num, c->desc_cnt);
		return NULL;
	}

	ds = kzalloc(sizeof(*ds), GFP_NOWAIT);
	if (!ds)
		return NULL;

	ds->chan = c;
	ds->desc_hw = dma_pool_zalloc(chan->desc_pool, GFP_ATOMIC,
				      &ds->desc_phys);
	if (!ds->desc_hw) {
		dev_dbg(d->dev, "out of memory for link descriptor\n");
		kfree(ds);
		return NULL;
	}
	ds->desc_cnt = num;

	return ds;
}

static void prep_slave_burst_len(struct ldma_chan *c)
{
	struct ldma_port *p = c->port;
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;
	struct dma_slave_config *cfg = &chan->config;

	if (cfg->dst_maxburst)
		cfg->src_maxburst = cfg->dst_maxburst;

	/* TX and RX has the same burst length */
	p->txbl = ilog2(cfg->src_maxburst);
	p->rxbl = p->txbl;
}

static struct dma_async_tx_descriptor *
cdma_prep_slave_sg(struct dma_chan *dma_chan, struct scatterlist *sgl,
		   unsigned int sglen, enum dma_transfer_direction dir,
		   unsigned long flags, void *context)
{
	struct ldma_chan *c = to_ldma_chan(dma_chan);
	struct cdma_chan *chan = (struct cdma_chan *)c->priv;
	size_t len, avail, total = 0;
	struct dw2_desc *hw_ds;
	struct dw2_desc_sw *ds;
	struct scatterlist *sg;
	int num = sglen, i;
	dma_addr_t addr;

	if (!sgl)
		return NULL;

	for_each_sg(sgl, sg, sglen, i) {
		avail = sg_dma_len(sg);
		if (avail > DMA_MAX_SIZE)
			num += DIV_ROUND_UP(avail, DMA_MAX_SIZE) - 1;
	}

	ds = cdma_alloc_desc_resource(num, c);
	if (!ds)
		return NULL;

	chan->ds = ds;

	num = 0;
	/* sop and eop has to be handled nicely */
	for_each_sg(sgl, sg, sglen, i) {
		addr = sg_dma_address(sg);
		avail = sg_dma_len(sg);
		total += avail;

		do {
			len = min_t(size_t, avail, DMA_MAX_SIZE);

			hw_ds = &ds->desc_hw[num];
			switch (sglen) {
			case 1:
				hw_ds->field &= ~DESC_SOP;
				hw_ds->field |= FIELD_PREP(DESC_SOP, 1);

				hw_ds->field &= ~DESC_EOP;
				hw_ds->field |= FIELD_PREP(DESC_EOP, 1);
				break;
			default:
				if (num == 0) {
					hw_ds->field &= ~DESC_SOP;
					hw_ds->field |= FIELD_PREP(DESC_SOP, 1);

					hw_ds->field &= ~DESC_EOP;
					hw_ds->field |= FIELD_PREP(DESC_EOP, 0);
				} else if (num == (sglen - 1)) {
					hw_ds->field &= ~DESC_SOP;
					hw_ds->field |= FIELD_PREP(DESC_SOP, 0);
					hw_ds->field &= ~DESC_EOP;
					hw_ds->field |= FIELD_PREP(DESC_EOP, 1);
				} else {
					hw_ds->field &= ~DESC_SOP;
					hw_ds->field |= FIELD_PREP(DESC_SOP, 0);

					hw_ds->field &= ~DESC_EOP;
					hw_ds->field |= FIELD_PREP(DESC_EOP, 0);
				}
				break;
			}
			/* Only 32 bit address supported */
			hw_ds->addr = (u32)addr;

			hw_ds->field &= ~DESC_DATA_LEN;
			hw_ds->field |= FIELD_PREP(DESC_DATA_LEN, len);

			hw_ds->field &= ~DESC_C;
			hw_ds->field |= FIELD_PREP(DESC_C, 0);

			hw_ds->field &= ~DESC_BYTE_OFF;
			hw_ds->field |= FIELD_PREP(DESC_BYTE_OFF, addr & 0x3);

			/* Ensure data ready before ownership change */
			wmb();
			hw_ds->field &= ~DESC_OWN;
			hw_ds->field |= FIELD_PREP(DESC_OWN, DMA_OWN);

			/* Ensure ownership changed before moving forward */
			wmb();
			num++;
			addr += len;
			avail -= len;
		} while (avail);
	}

	ds->size = total;
	prep_slave_burst_len(c);

	return vchan_tx_prep(&c->vchan, &ds->vdesc, DMA_CTRL_ACK);
}

static void cdma_func_init(struct ldma_dev *d, struct dma_device *dma_dev)
{
	dma_dev->device_alloc_chan_resources = cdma_alloc_chan_resources;
	dma_dev->device_free_chan_resources = cdma_free_chan_resources;
	dma_dev->device_terminate_all = ldma_terminate_all;
	dma_dev->device_issue_pending = cdma_issue_pending;
	dma_dev->device_tx_status = cdma_tx_status;
	dma_dev->device_resume = ldma_resume_chan;
	dma_dev->device_pause = ldma_pause_chan;
	dma_dev->device_prep_slave_sg = cdma_prep_slave_sg;

	dma_dev->device_config = cdma_slave_config;
	dma_dev->device_synchronize = cdma_synchronize;
	dma_dev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma_dev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dma_dev->directions = BIT(DMA_MEM_TO_DEV) | BIT(DMA_DEV_TO_MEM);
	dma_dev->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
}
