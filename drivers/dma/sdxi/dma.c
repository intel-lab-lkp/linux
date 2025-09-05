// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI DMA engine implementation
 *   Derived from ptdma code
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>

#include "../dmaengine.h"
#include "context.h"
#include "descriptor.h"
#include "dma.h"
#include "sdxi.h"

struct sdxi_dma_desc {
	struct virt_dma_desc vd;
	struct sdxi_cxt *cxt;
	enum dma_status status;
	bool issued_to_hw;
	struct sdxi_cmd sdxi_cmd;
};

static inline struct sdxi_dma_chan *to_sdxi_dma_chan(struct dma_chan *dma_chan)
{
	return container_of(dma_chan, struct sdxi_dma_chan, vc.chan);
}

static inline struct sdxi_dma_desc *to_sdxi_dma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct sdxi_dma_desc, vd);
}

static void sdxi_dma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);

	vchan_free_chan_resources(&chan->vc);
	/* NB: more configure with sdxi_cxt? */
}

static void sdxi_dma_synchronize(struct dma_chan *c)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(c);

	vchan_synchronize(&chan->vc);
}

static void sdxi_do_cleanup(struct virt_dma_desc *vd)
{
	struct sdxi_dma_desc *dma_desc = to_sdxi_dma_desc(vd);
	struct sdxi_cmd *cmd = &dma_desc->sdxi_cmd;
	struct device *dev = sdxi_to_dev(dma_desc->cxt->sdxi);

	dma_free_coherent(dev, sizeof(*cmd->cst_blk),
			  cmd->cst_blk, cmd->cst_blk_dma);
	kfree(dma_desc);
}

static int sdxi_dma_start_desc(struct sdxi_dma_desc *dma_desc)
{
	struct sdxi_dev *sdxi;
	struct sdxi_cmd *sdxi_cmd;
	struct sdxi_cxt *cxt;
	struct sdxi_desc desc;
	struct sdxi_copy copy;
	struct sdxi_cst_blk *cst_blk;
	dma_addr_t cst_blk_dma;
	int err;

	sdxi_cmd = &dma_desc->sdxi_cmd;
	sdxi = sdxi_cmd->cxt->sdxi;

	cxt = dma_desc->cxt;

	if (sdxi_cmd->len > MAX_DMA_COPY_BYTES)
		return -EINVAL;

	copy = (typeof(copy)) {
		.src = sdxi_cmd->src_addr,
		.dst = sdxi_cmd->dst_addr,
		.src_akey = 0,
		.dst_akey = 0,
		.len = sdxi_cmd->len,
	};

	err = sdxi_encode_copy(&desc, &copy);
	if (err)
		return err;

	err = sdxi_encode_copy(&desc, &copy);
	if (err)
		return err;

	/* FIXME convert to pool */
	cst_blk = dma_alloc_coherent(sdxi_to_dev(sdxi), sizeof(*cst_blk),
				     &cst_blk_dma, GFP_NOWAIT);
	if (!cst_blk)
		return -ENOMEM;

	cst_blk->signal = cpu_to_le64(0xff);

	sdxi_cmd->cst_blk = cst_blk;
	sdxi_cmd->cst_blk_dma = cst_blk_dma;
	sdxi_cmd->ret = 0; /* TODO: get desc submit status & update ret value */

	sdxi_desc_set_csb(&desc, cst_blk_dma);
	err = sdxi_submit_desc(cxt, &desc);
	if (err)
		goto free_cst_blk;

	sdxi->tdata.cmd = sdxi_cmd; /* FIXME: this is not compatible w/multiple clients */
	dma_desc->issued_to_hw = 1;
	return 0;
free_cst_blk:
	dma_free_coherent(sdxi_to_dev(sdxi), sizeof(*cst_blk),
			  cst_blk, cst_blk_dma);
	return err;
}

static struct sdxi_dma_desc *sdxi_next_dma_desc(struct sdxi_dma_chan *chan)
{
	/* Get the next DMA descriptor on the active list */
	struct virt_dma_desc *vd = vchan_next_desc(&chan->vc);

	return vd ? to_sdxi_dma_desc(vd) : NULL;
}

static struct sdxi_dma_desc *sdxi_handle_active_desc(struct sdxi_dma_chan *chan,
						     struct sdxi_dma_desc *desc)
{
	struct dma_async_tx_descriptor *tx_desc;
	struct virt_dma_desc *vd;
	unsigned long flags;

	/* Loop over descriptors until one is found with commands */
	do {
		if (desc) {
			if (!desc->issued_to_hw) {
				/* No errors, keep going */
				if (desc->status != DMA_ERROR)
					return desc;
			}

			tx_desc = &desc->vd.tx;
			vd = &desc->vd;
		} else {
			tx_desc = NULL;
		}

		spin_lock_irqsave(&chan->vc.lock, flags);

		if (desc) {

			if (desc->status != DMA_COMPLETE) {
				if (desc->status != DMA_ERROR)
					desc->status = DMA_COMPLETE;

				dma_cookie_complete(tx_desc);
				dma_descriptor_unmap(tx_desc);
				list_del(&desc->vd.node);
			} else {
				/* Don't handle it twice */
				tx_desc = NULL;
			}
		}

		desc = sdxi_next_dma_desc(chan);

		spin_unlock_irqrestore(&chan->vc.lock, flags);

		if (tx_desc) {
			dmaengine_desc_get_callback_invoke(tx_desc, NULL);
			dma_run_dependencies(tx_desc);
			vchan_vdesc_fini(vd);
		}
	} while (desc);

	return NULL;
}

static void sdxi_cmd_callback(void *data, int err)
{
	struct sdxi_dma_desc *desc = data;
	struct dma_chan *dma_chan;
	struct sdxi_dma_chan *chan;
	int ret;

	if (err == -EINPROGRESS)
		return;

	dma_chan = desc->vd.tx.chan;
	chan = to_sdxi_dma_chan(dma_chan);

	if (err)
		desc->status = DMA_ERROR;

	while (true) {
		/* Check for DMA descriptor completion */
		desc = sdxi_handle_active_desc(chan, desc);

		/* Don't submit cmd if no descriptor or DMA is paused */
		if (!desc)
			break;

		ret = sdxi_dma_start_desc(desc);
		if (!ret)
			break;

		desc->status = DMA_ERROR;
	}
}

static struct sdxi_dma_desc *sdxi_dma_alloc_dma_desc(struct sdxi_dma_chan *chan,
						     unsigned long flags)
{
	struct sdxi_dma_desc *desc;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->cxt = chan->cxt;

	vchan_tx_prep(&chan->vc, &desc->vd, flags);

	desc->cxt->sdxi = chan->cxt->sdxi;
	desc->issued_to_hw = 0;
	desc->status = DMA_IN_PROGRESS;

	return desc;
}

static struct sdxi_dma_desc *sdxi_dma_create_desc(struct dma_chan *dma_chan,
						  dma_addr_t dst,
						  dma_addr_t src,
						  unsigned int len,
						  unsigned long flags)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);
	struct sdxi_dma_desc *desc;
	struct sdxi_cmd *sdxi_cmd;

	desc = sdxi_dma_alloc_dma_desc(chan, flags);
	if (!desc)
		return NULL;

	sdxi_cmd = &desc->sdxi_cmd;
	sdxi_cmd->cxt = chan->cxt;
	sdxi_cmd->cxt->sdxi = chan->cxt->sdxi;
	sdxi_cmd->src_addr = src;
	sdxi_cmd->dst_addr = dst;
	sdxi_cmd->len = len;
	sdxi_cmd->sdxi_cmd_callback = sdxi_cmd_callback;
	sdxi_cmd->data = desc;

	return desc;
}

static struct dma_async_tx_descriptor *
sdxi_dma_prep_memcpy(struct dma_chan *dma_chan, dma_addr_t dst,
		     dma_addr_t src, size_t len, unsigned long flags)
{
	struct sdxi_dma_desc *desc;

	desc = sdxi_dma_create_desc(dma_chan, dst, src, len, flags);
	if (!desc)
		return NULL;

	return &desc->vd.tx;
}

static struct dma_async_tx_descriptor *
sdxi_prep_dma_interrupt(struct dma_chan *dma_chan, unsigned long flags)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);
	struct sdxi_dma_desc *desc;

	desc = sdxi_dma_alloc_dma_desc(chan, flags);
	if (!desc)
		return NULL;

	return &desc->vd.tx;
}

static void sdxi_dma_issue_pending(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);
	struct sdxi_dma_desc *desc;
	unsigned long flags;
	bool engine_is_idle = true;

	spin_lock_irqsave(&chan->vc.lock, flags);

	desc = sdxi_next_dma_desc(chan);
	if (desc)
		engine_is_idle = false;

	vchan_issue_pending(&chan->vc);

	desc = sdxi_next_dma_desc(chan);

	spin_unlock_irqrestore(&chan->vc.lock, flags);

	/* If there was nothing active, start processing */
	if (engine_is_idle)
		sdxi_cmd_callback(desc, 0);
}

static void sdxi_check_trans_status(struct sdxi_dma_chan *chan)
{
	struct sdxi_cxt *cxt = chan->cxt;
	struct sdxi_cmd *cmd;

	if (!cxt)
		return;

	cmd = cxt->sdxi->tdata.cmd;

	if (le64_to_cpu(cmd->cst_blk->signal) == 0xfe)
		sdxi_cmd_callback(cmd->data, cmd->ret);
}

static enum dma_status sdxi_tx_status(struct dma_chan *dma_chan, dma_cookie_t cookie,
				      struct dma_tx_state *tx_state)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);

	sdxi_check_trans_status(chan);

	return dma_cookie_status(dma_chan, cookie, tx_state);
}

static int sdxi_dma_terminate_all(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *chan = to_sdxi_dma_chan(dma_chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vc.lock, flags);
	vchan_get_all_descriptors(&chan->vc, &head);
	spin_unlock_irqrestore(&chan->vc.lock, flags);

	vchan_dma_desc_free_list(&chan->vc, &head);
	vchan_free_chan_resources(&chan->vc);

	return 0;
}

int sdxi_dma_register(struct sdxi_cxt *dma_cxt)
{
	struct sdxi_dma_chan *chan;
	struct sdxi_dev *sdxi = dma_cxt->sdxi;
	struct device *dev = sdxi_to_dev(sdxi);
	struct dma_device *dma_dev = &sdxi->dma_dev;
	int ret = 0;

	sdxi->sdxi_dma_chan = devm_kzalloc(dev, sizeof(*sdxi->sdxi_dma_chan),
					   GFP_KERNEL);
	if (!sdxi->sdxi_dma_chan)
		return -ENOMEM;

	sdxi->sdxi_dma_chan->cxt = dma_cxt;

	dma_dev->dev = dev;
	dma_dev->src_addr_widths = DMA_SLAVE_BUSWIDTH_64_BYTES;
	dma_dev->dst_addr_widths = DMA_SLAVE_BUSWIDTH_64_BYTES;
	dma_dev->directions = BIT(DMA_MEM_TO_MEM);
	dma_dev->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);
	dma_cap_set(DMA_INTERRUPT, dma_dev->cap_mask);

	dma_cap_set(DMA_PRIVATE, dma_dev->cap_mask);

	INIT_LIST_HEAD(&dma_dev->channels);

	chan = sdxi->sdxi_dma_chan;
	chan->cxt->sdxi = sdxi;

	/* Set base and prep routines */
	dma_dev->device_free_chan_resources = sdxi_dma_free_chan_resources;
	dma_dev->device_prep_dma_memcpy = sdxi_dma_prep_memcpy;
	dma_dev->device_prep_dma_interrupt = sdxi_prep_dma_interrupt;
	dma_dev->device_issue_pending = sdxi_dma_issue_pending;
	dma_dev->device_tx_status = sdxi_tx_status;
	dma_dev->device_terminate_all = sdxi_dma_terminate_all;
	dma_dev->device_synchronize = sdxi_dma_synchronize;

	chan->vc.desc_free = sdxi_do_cleanup;
	vchan_init(&chan->vc, dma_dev);

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));

	ret = dma_async_device_register(dma_dev);
	if (ret)
		goto err_reg;

	return 0;

err_reg:
	return ret;
}

void sdxi_dma_unregister(struct sdxi_cxt *dma_cxt)
{
	dma_async_device_unregister(&dma_cxt->sdxi->dma_dev);
}
