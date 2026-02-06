// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
/*
 * Copyright (C) 2025 Amlogic, Inc. All rights reserved
 * Author: Xianwei Zhao <xianwei.zhao@amlogic.com>
 */

#include <asm/irq.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "dmaengine.h"

#define RCH_REG_BASE		0x0
#define WCH_REG_BASE		0x2000
/*
 * Each rch (read from memory) REG offset  Rch_offset 0x0 each channel total 0x40
 * rch addr = DMA_base + Rch_offset+ chan_id * 0x40 + reg_offset
 */
#define RCH_READY		0x0
#define RCH_STATUS		0x4
#define RCH_CFG			0x8
#define CFG_CLEAR		BIT(25)
#define CFG_PAUSE		BIT(26)
#define CFG_ENABLE		BIT(27)
#define CFG_DONE		BIT(28)
#define RCH_ADDR		0xc
#define RCH_LEN			0x10
#define RCH_RD_LEN		0x14
#define RCH_PRT			0x18
#define RCH_SYCN_STAT		0x1c
#define RCH_ADDR_LOW		0x20
#define RCH_ADDR_HIGH		0x24
/* if work on 64, it work with RCH_PRT */
#define RCH_PTR_HIGH		0x28

/*
 * Each wch (write to memory) REG offset  Wch_offset 0x2000 each channel total 0x40
 * wch addr = DMA_base + Wch_offset+ chan_id * 0x40 + reg_offset
 */
#define WCH_READY		0x0
#define WCH_TOTAL_LEN		0x4
#define WCH_CFG			0x8
#define WCH_ADDR		0xc
#define WCH_LEN			0x10
#define WCH_RD_LEN		0x14
#define WCH_PRT			0x18
#define WCH_CMD_CNT		0x1c
#define WCH_ADDR_LOW		0x20
#define WCH_ADDR_HIGH		0x24
/* if work on 64, it work with RCH_PRT */
#define WCH_PTR_HIGH		0x28

/* DMA controller reg */
#define RCH_INT_MASK		0x1000
#define WCH_INT_MASK		0x1004
#define CLEAR_W_BATCH		0x1014
#define CLEAR_RCH		0x1024
#define CLEAR_WCH		0x1028
#define RCH_ACTIVE		0x1038
#define WCH_ACTIVE		0x103c
#define RCH_DONE		0x104c
#define WCH_DONE		0x1050
#define RCH_ERR			0x1060
#define RCH_LEN_ERR		0x1064
#define WCH_ERR			0x1068
#define DMA_BATCH_END		0x1078
#define WCH_EOC_DONE		0x1088
#define WDMA_RESP_ERR		0x1098
#define UPT_PKT_SYNC		0x10a8
#define RCHN_CFG		0x10ac
#define WCHN_CFG		0x10b0
#define MEM_PD_CFG		0x10b4
#define MEM_BUS_CFG		0x10b8
#define DMA_GMV_CFG		0x10bc
#define DMA_GMR_CFG		0x10c0

#define AML_DMA_TYPE_TX		0
#define AML_DMA_TYPE_RX		1
#define DMA_MAX_LINK		8
#define MAX_CHAN_ID		32
#define SG_MAX_LEN		GENMASK(26, 0)

struct aml_dma_sg_link {
#define LINK_LEN		GENMASK(26, 0)
#define LINK_IRQ		BIT(27)
#define LINK_EOC		BIT(28)
#define LINK_LOOP		BIT(29)
#define LINK_ERR		BIT(30)
#define LINK_OWNER		BIT(31)
	u32 ctl;
	u64 address;
	u32 revered;
} __packed;

struct aml_dma_chan {
	struct dma_chan			chan;
	struct dma_async_tx_descriptor	desc;
	struct aml_dma_dev		*aml_dma;
	struct aml_dma_sg_link		*sg_link;
	dma_addr_t			sg_link_phys;
	int				sg_link_cnt;
	int				data_len;
	enum dma_status			pre_status;
	enum dma_status			status;
	enum dma_transfer_direction	direction;
	int				chan_id;
	/* reg_base (direction + chan_id) */
	int				reg_offs;
};

struct aml_dma_dev {
	struct dma_device		dma_device;
	void __iomem			*base;
	struct regmap			*regmap;
	struct clk			*clk;
	int				irq;
	struct platform_device		*pdev;
	struct aml_dma_chan		*aml_rch[MAX_CHAN_ID];
	struct aml_dma_chan		*aml_wch[MAX_CHAN_ID];
	unsigned int			chan_nr;
	unsigned int			chan_used;
	struct aml_dma_chan		aml_chans[]__counted_by(chan_nr);
};

static struct aml_dma_chan *to_aml_dma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct aml_dma_chan, chan);
}

static dma_cookie_t aml_dma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	return dma_cookie_assign(tx);
}

static int aml_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;
	size_t size = size_mul(sizeof(struct aml_dma_sg_link), DMA_MAX_LINK);

	aml_chan->sg_link = dma_alloc_coherent(aml_dma->dma_device.dev, size,
					       &aml_chan->sg_link_phys, GFP_KERNEL);
	if (!aml_chan->sg_link)
		return  -ENOMEM;

	/* offset is the same RCH_CFG and WCH_CFG */
	regmap_update_bits(aml_dma->regmap, aml_chan->reg_offs + RCH_CFG, CFG_CLEAR, CFG_CLEAR);
	aml_chan->status = DMA_COMPLETE;
	dma_async_tx_descriptor_init(&aml_chan->desc, chan);
	aml_chan->desc.tx_submit = aml_dma_tx_submit;
	regmap_update_bits(aml_dma->regmap, aml_chan->reg_offs + RCH_CFG, CFG_CLEAR, 0);

	return 0;
}

static void aml_dma_free_chan_resources(struct dma_chan *chan)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;

	aml_chan->status = DMA_COMPLETE;
	dma_free_coherent(aml_dma->dma_device.dev,
			  sizeof(struct aml_dma_sg_link) * DMA_MAX_LINK,
			  aml_chan->sg_link, aml_chan->sg_link_phys);
}

/* DMA transfer state  update how many data reside it */
static enum dma_status aml_dma_tx_status(struct dma_chan *chan,
					 dma_cookie_t cookie,
					 struct dma_tx_state *txstate)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;
	u32 residue, done;

	regmap_read(aml_dma->regmap, aml_chan->reg_offs + RCH_RD_LEN, &done);
	residue = aml_chan->data_len - done;
	dma_set_tx_state(txstate, chan->completed_cookie, chan->cookie,
			 residue);

	return aml_chan->status;
}

static struct dma_async_tx_descriptor *aml_dma_prep_slave_sg
		(struct dma_chan *chan, struct scatterlist *sgl,
		unsigned int sg_len, enum dma_transfer_direction direction,
		unsigned long flags, void *context)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;
	struct aml_dma_sg_link *sg_link;
	struct scatterlist *sg;
	int idx = 0;
	u32 reg, chan_id;
	u32 i;

	if (aml_chan->direction != direction) {
		dev_err(aml_dma->dma_device.dev, "direction not support\n");
		return NULL;
	}

	switch (aml_chan->status) {
	case DMA_IN_PROGRESS:
		dev_err(aml_dma->dma_device.dev, "not support multi tx_desciptor\n");
		return NULL;

	case DMA_COMPLETE:
		aml_chan->data_len = 0;
		chan_id = aml_chan->chan_id;
		reg = (direction == DMA_DEV_TO_MEM) ? WCH_INT_MASK : RCH_INT_MASK;
		regmap_update_bits(aml_dma->regmap, reg, BIT(chan_id), BIT(chan_id));

		break;
	default:
		dev_err(aml_dma->dma_device.dev, "status error\n");
		return NULL;
	}

	if (sg_len > DMA_MAX_LINK) {
		dev_err(aml_dma->dma_device.dev,
			"maximum number of sg exceeded: %d > %d\n",
			sg_len, DMA_MAX_LINK);
		aml_chan->status = DMA_ERROR;
		return NULL;
	}

	aml_chan->status = DMA_IN_PROGRESS;

	for_each_sg(sgl, sg, sg_len, i) {
		if (sg_dma_len(sg) > SG_MAX_LEN) {
			dev_err(aml_dma->dma_device.dev,
				"maximum bytes exceeded: %u > %lu\n",
				sg_dma_len(sg), SG_MAX_LEN);
			aml_chan->status = DMA_ERROR;
			return NULL;
		}
		sg_link = &aml_chan->sg_link[idx++];
		/* set dma address and len  to sglink*/
		sg_link->address = sg->dma_address;
		sg_link->ctl = FIELD_PREP(LINK_LEN, sg_dma_len(sg));

		aml_chan->data_len += sg_dma_len(sg);
	}
	aml_chan->sg_link_cnt = idx;

	return &aml_chan->desc;
}

static int aml_dma_pause_chan(struct dma_chan *chan)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;

	regmap_update_bits(aml_dma->regmap, aml_chan->reg_offs + RCH_CFG, CFG_PAUSE, CFG_PAUSE);
	aml_chan->pre_status = aml_chan->status;
	aml_chan->status = DMA_PAUSED;

	return 0;
}

static int aml_dma_resume_chan(struct dma_chan *chan)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;

	regmap_update_bits(aml_dma->regmap, aml_chan->reg_offs + RCH_CFG, CFG_PAUSE, 0);
	aml_chan->status = aml_chan->pre_status;

	return 0;
}

static int aml_dma_terminate_all(struct dma_chan *chan)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;
	int chan_id = aml_chan->chan_id;

	aml_dma_pause_chan(chan);
	regmap_update_bits(aml_dma->regmap, aml_chan->reg_offs + RCH_CFG, CFG_CLEAR, CFG_CLEAR);

	if (aml_chan->direction == DMA_MEM_TO_DEV)
		regmap_update_bits(aml_dma->regmap, RCH_INT_MASK, BIT(chan_id), BIT(chan_id));
	else if (aml_chan->direction == DMA_DEV_TO_MEM)
		regmap_update_bits(aml_dma->regmap, WCH_INT_MASK, BIT(chan_id), BIT(chan_id));

	aml_chan->status = DMA_COMPLETE;

	return 0;
}

static void aml_dma_enable_chan(struct dma_chan *chan)
{
	struct aml_dma_chan *aml_chan = to_aml_dma_chan(chan);
	struct aml_dma_dev *aml_dma = aml_chan->aml_dma;
	struct aml_dma_sg_link *sg_link;
	int chan_id = aml_chan->chan_id;
	int idx = aml_chan->sg_link_cnt - 1;

	/* the last sg set eoc flag */
	sg_link = &aml_chan->sg_link[idx];
	sg_link->ctl |= LINK_EOC;
	if (aml_chan->direction == DMA_MEM_TO_DEV) {
		regmap_write(aml_dma->regmap, aml_chan->reg_offs + RCH_ADDR,
			     aml_chan->sg_link_phys);
		regmap_write(aml_dma->regmap, aml_chan->reg_offs + RCH_LEN, aml_chan->data_len);
		regmap_update_bits(aml_dma->regmap, RCH_INT_MASK, BIT(chan_id), 0);
		/* for rch (tx) need set cfg 0 to trigger start */
		regmap_write(aml_dma->regmap, aml_chan->reg_offs + RCH_CFG, 0);
	} else if (aml_chan->direction == DMA_DEV_TO_MEM) {
		regmap_write(aml_dma->regmap, aml_chan->reg_offs + WCH_ADDR,
			     aml_chan->sg_link_phys);
		regmap_write(aml_dma->regmap, aml_chan->reg_offs + WCH_LEN, aml_chan->data_len);
		regmap_update_bits(aml_dma->regmap, WCH_INT_MASK, BIT(chan_id), 0);
	}
}

static irqreturn_t aml_dma_interrupt_handler(int irq, void *dev_id)
{
	struct aml_dma_dev *aml_dma = dev_id;
	struct aml_dma_chan *aml_chan;
	u32 done, eoc_done, err, err_l, end;
	int i = 0;

	/* deal with rch normal complete and error */
	regmap_read(aml_dma->regmap, RCH_DONE, &done);
	regmap_read(aml_dma->regmap, RCH_ERR, &err);
	regmap_read(aml_dma->regmap, RCH_LEN_ERR, &err_l);
	err = err | err_l;

	done = done | err;

	while (done) {
		i = ffs(done) - 1;
		aml_chan = aml_dma->aml_rch[i];
		regmap_write(aml_dma->regmap, CLEAR_RCH, BIT(aml_chan->chan_id));
		if (!aml_chan) {
			dev_err(aml_dma->dma_device.dev, "idx %d rch not initialized\n", i);
			done &= ~BIT(i);
			continue;
		}
		aml_chan->status = (err & (1 << i)) ? DMA_ERROR : DMA_COMPLETE;
		dma_cookie_complete(&aml_chan->desc);
		dmaengine_desc_get_callback_invoke(&aml_chan->desc, NULL);
		done &= ~BIT(i);
	}

	/* deal with wch normal complete and error */
	regmap_read(aml_dma->regmap, DMA_BATCH_END, &end);
	if (end)
		regmap_write(aml_dma->regmap, CLEAR_W_BATCH, end);

	regmap_read(aml_dma->regmap, WCH_DONE, &done);
	regmap_read(aml_dma->regmap, WCH_EOC_DONE, &eoc_done);
	done = done | eoc_done;

	regmap_read(aml_dma->regmap, WCH_ERR, &err);
	regmap_read(aml_dma->regmap, WDMA_RESP_ERR, &err_l);
	err = err | err_l;

	done = done | err;
	i = 0;
	while (done) {
		i = ffs(done) - 1;
		aml_chan = aml_dma->aml_wch[i];
		regmap_write(aml_dma->regmap, CLEAR_WCH, BIT(aml_chan->chan_id));
		if (!aml_chan) {
			dev_err(aml_dma->dma_device.dev, "idx %d wch not initialized\n", i);
			done &= ~BIT(i);
			continue;
		}
		aml_chan->status = (err & (1 << i)) ? DMA_ERROR : DMA_COMPLETE;
		dma_cookie_complete(&aml_chan->desc);
		dmaengine_desc_get_callback_invoke(&aml_chan->desc, NULL);
		done &= ~BIT(i);
	}

	return IRQ_HANDLED;
}

static struct dma_chan *aml_of_dma_xlate(struct of_phandle_args *dma_spec, struct of_dma *ofdma)
{
	struct aml_dma_dev *aml_dma = (struct aml_dma_dev *)ofdma->of_dma_data;
	struct aml_dma_chan *aml_chan = NULL;
	u32 type;
	u32 phy_chan_id;

	if (dma_spec->args_count != 2)
		return NULL;

	type = dma_spec->args[0];
	phy_chan_id = dma_spec->args[1];

	if (phy_chan_id >= MAX_CHAN_ID)
		return NULL;

	if (type == AML_DMA_TYPE_TX) {
		aml_chan = aml_dma->aml_rch[phy_chan_id];
		if (!aml_chan) {
			if (aml_dma->chan_used >= aml_dma->chan_nr) {
				dev_err(aml_dma->dma_device.dev, "some dma clients err used\n");
				return NULL;
			}
			aml_chan = &aml_dma->aml_chans[aml_dma->chan_used];
			aml_dma->chan_used++;
			aml_chan->direction = DMA_MEM_TO_DEV;
			aml_chan->chan_id = phy_chan_id;
			aml_chan->reg_offs = RCH_REG_BASE + 0x40 * aml_chan->chan_id;
			aml_dma->aml_rch[phy_chan_id] = aml_chan;
		}
	} else if (type == AML_DMA_TYPE_RX) {
		aml_chan = aml_dma->aml_wch[phy_chan_id];
		if (!aml_chan) {
			if (aml_dma->chan_used >= aml_dma->chan_nr) {
				dev_err(aml_dma->dma_device.dev, "some dma clients err used\n");
				return NULL;
			}
			aml_chan = &aml_dma->aml_chans[aml_dma->chan_used];
			aml_dma->chan_used++;
			aml_chan->direction = DMA_DEV_TO_MEM;
			aml_chan->chan_id = phy_chan_id;
			aml_chan->reg_offs = WCH_REG_BASE + 0x40 * aml_chan->chan_id;
			aml_dma->aml_wch[phy_chan_id] = aml_chan;
		}
	} else {
		dev_err(aml_dma->dma_device.dev, "type %d not supported\n", type);
		return NULL;
	}

	return dma_get_slave_channel(&aml_chan->chan);
}

static int aml_dma_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct dma_device *dma_dev;
	struct aml_dma_dev *aml_dma;
	int ret, i, len;
	u32 chan_nr;

	const struct regmap_config aml_regmap_config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = 0x3000,
	};

	ret = of_property_read_u32(np, "dma-channels", &chan_nr);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to read dma-channels\n");

	len = sizeof(*aml_dma) + sizeof(struct aml_dma_chan) * chan_nr;
	aml_dma = devm_kzalloc(&pdev->dev, len, GFP_KERNEL);
	if (!aml_dma)
		return -ENOMEM;

	aml_dma->chan_nr = chan_nr;

	aml_dma->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(aml_dma->base))
		return PTR_ERR(aml_dma->base);

	aml_dma->regmap = devm_regmap_init_mmio(&pdev->dev, aml_dma->base,
						&aml_regmap_config);
	if (IS_ERR_OR_NULL(aml_dma->regmap))
		return PTR_ERR(aml_dma->regmap);

	aml_dma->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(aml_dma->clk))
		return PTR_ERR(aml_dma->clk);

	aml_dma->irq = platform_get_irq(pdev, 0);

	aml_dma->pdev = pdev;
	aml_dma->dma_device.dev = &pdev->dev;

	dma_dev = &aml_dma->dma_device;
	INIT_LIST_HEAD(&dma_dev->channels);

	/* Initialize channel parameters */
	for (i = 0; i < chan_nr; i++) {
		struct aml_dma_chan *aml_chan = &aml_dma->aml_chans[i];

		aml_chan->aml_dma = aml_dma;
		aml_chan->chan.device = &aml_dma->dma_device;
		dma_cookie_init(&aml_chan->chan);

		/* Add the channel to aml_chan list */
		list_add_tail(&aml_chan->chan.device_node,
			      &aml_dma->dma_device.channels);
	}
	aml_dma->chan_used = 0;

	dma_set_max_seg_size(dma_dev->dev, SG_MAX_LEN);

	dma_cap_set(DMA_SLAVE, dma_dev->cap_mask);
	dma_dev->device_alloc_chan_resources = aml_dma_alloc_chan_resources;
	dma_dev->device_free_chan_resources = aml_dma_free_chan_resources;
	dma_dev->device_tx_status = aml_dma_tx_status;
	dma_dev->device_prep_slave_sg = aml_dma_prep_slave_sg;

	dma_dev->device_pause = aml_dma_pause_chan;
	dma_dev->device_resume = aml_dma_resume_chan;
	dma_dev->device_terminate_all = aml_dma_terminate_all;
	dma_dev->device_issue_pending = aml_dma_enable_chan;
	/* PIO 4 bytes and I2C 1 byte */
	dma_dev->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES | DMA_SLAVE_BUSWIDTH_1_BYTE);
	dma_dev->directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	dma_dev->residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;

	ret = dmaenginem_async_device_register(dma_dev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to register dmaenginem\n");

	ret = of_dma_controller_register(np, aml_of_dma_xlate, aml_dma);
	if (ret)
		return ret;

	regmap_write(aml_dma->regmap, RCH_INT_MASK, 0xffffffff);
	regmap_write(aml_dma->regmap, WCH_INT_MASK, 0xffffffff);

	ret = devm_request_irq(&pdev->dev, aml_dma->irq, aml_dma_interrupt_handler,
			       IRQF_SHARED, dev_name(&pdev->dev), aml_dma);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to reqest_irq\n");

	return 0;
}

static const struct of_device_id aml_dma_ids[] = {
	{ .compatible = "amlogic,a9-dma", },
	{},
};
MODULE_DEVICE_TABLE(of, aml_dma_ids);

static struct platform_driver aml_dma_driver = {
	.probe = aml_dma_probe,
	.driver		= {
		.name	= "aml-dma",
		.of_match_table = aml_dma_ids,
	},
};

module_platform_driver(aml_dma_driver);

MODULE_DESCRIPTION("GENERAL DMA driver for Amlogic");
MODULE_AUTHOR("Xianwei Zhao <xianwei.zhao@amlogic.com>");
MODULE_LICENSE("GPL");
