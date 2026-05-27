// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Andes ATCDMAC300 controller driver
 *
 * Copyright (C) 2025 Andes Technology Corporation
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/sprintf.h>
#include <linux/regmap.h>
#include "dmaengine.h"
#include "atcdmac300.h"

static int atcdmac_is_chan_enable(struct atcdmac_chan *dmac_chan)
{
	struct atcdmac_dmac *dmac =
		atcdmac_dev_to_dmac(dmac_chan->dma_chan.device);

	return regmap_test_bits(dmac->regmap,
				REG_CH_EN,
				BIT(dmac_chan->chan_id));
}

static void atcdmac_enable_chan(struct atcdmac_chan *dmac_chan, bool enable)
{
	regmap_update_bits(dmac_chan->regmap, REG_CH_CTL_OFF, CHEN, enable);
}

static void atcdmac_abort_chan(struct atcdmac_chan *dmac_chan)
{
	regmap_write_bits(dmac_chan->dma_dev->regmap,
			  REG_CH_ABT,
			  BIT(dmac_chan->chan_id),
			  BIT(dmac_chan->chan_id));
}

static dma_cookie_t atcdmac_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(tx->chan);
	struct atcdmac_desc *desc = atcdmac_txd_to_dma_desc(tx);
	dma_cookie_t cookie;
	unsigned long flags;

	spin_lock_irqsave(&dmac_chan->lock, flags);
	cookie = dma_cookie_assign(tx);
	list_add_tail(&desc->desc_node, &dmac_chan->queue_list);
	spin_unlock_irqrestore(&dmac_chan->lock, flags);

	return cookie;
}

static struct atcdmac_desc *
atcdmac_get_active_head(struct atcdmac_chan *dmac_chan)
{
	return list_first_entry(&dmac_chan->active_list,
				struct atcdmac_desc,
				desc_node);
}

static struct atcdmac_desc *atcdmac_alloc_desc(struct dma_chan *chan,
					       gfp_t gfp_flags)
{
	struct atcdmac_dmac *dmac = atcdmac_dev_to_dmac(chan->device);
	struct atcdmac_desc *desc;
	dma_addr_t phys;

	desc = dma_pool_zalloc(dmac->dma_desc_pool, gfp_flags, &phys);
	if (desc) {
		INIT_LIST_HEAD(&desc->tx_list);
		dma_async_tx_descriptor_init(&desc->txd, chan);
		desc->txd.flags = DMA_CTRL_ACK;
		desc->txd.tx_submit = atcdmac_tx_submit;
		desc->txd.phys = phys;
	}

	return desc;
}

static struct atcdmac_desc *atcdmac_get_desc(struct atcdmac_chan *dmac_chan)
{
	struct atcdmac_desc *ret = NULL;
	struct atcdmac_desc *desc_next;
	struct atcdmac_desc *desc;
	unsigned long flags;

	spin_lock_irqsave(&dmac_chan->lock, flags);
	list_for_each_entry_safe(desc, desc_next,
				 &dmac_chan->free_list,
				 desc_node) {
		if (async_tx_test_ack(&desc->txd)) {
			list_del_init(&desc->desc_node);
			ret = desc;
			break;
		}
	}
	spin_unlock_irqrestore(&dmac_chan->lock, flags);

	if (!ret) {
		ret = atcdmac_alloc_desc(&dmac_chan->dma_chan, GFP_ATOMIC);
		if (ret) {
			spin_lock_irqsave(&dmac_chan->lock, flags);
			dmac_chan->descs_allocated++;
			spin_unlock_irqrestore(&dmac_chan->lock, flags);
		} else {
			dev_warn(atcdmac_chan_to_dev(&dmac_chan->dma_chan),
				 "not enough descriptors available\n");
		}
	}

	return ret;
}

/**
 * atcdmac_put_desc_nolock - move a descriptor to the free list
 * @dmac_chan: DMA channel we work on
 * @desc: Head of the descriptor chain to be added to the free list
 *
 * This function does not use a lock to protect any linked lists in
 * 'struct atcdmac_chan', so please remember to add a proper lock when
 * calling the function.
 */
static void atcdmac_put_desc_nolock(struct atcdmac_chan *dmac_chan,
				    struct atcdmac_desc *desc)
{
	struct atcdmac_desc *child, *tmp;

	if (desc) {
		list_for_each_entry_safe(child,
					 tmp,
					 &desc->tx_list,
					 desc_node) {
			list_del_init(&child->desc_node);
			child->at = NULL;
			child->num_sg = 0;
			INIT_LIST_HEAD(&child->tx_list);
			list_add_tail(&child->desc_node,
				      &dmac_chan->free_list);
		}

		list_del_init(&desc->desc_node);
		desc->at = NULL;
		desc->num_sg = 0;
		INIT_LIST_HEAD(&desc->tx_list);
		list_add_tail(&desc->desc_node, &dmac_chan->free_list);
	}
}

static void atcdmac_put_desc(struct atcdmac_chan *dmac_chan,
			     struct atcdmac_desc *desc)
{
	unsigned long flags;

	if (!desc) {
		dev_err(atcdmac_chan_to_dev(&dmac_chan->dma_chan),
			"A NULL descriptor was found.\n");
		return;
	}

	spin_lock_irqsave(&dmac_chan->lock, flags);
	atcdmac_put_desc_nolock(dmac_chan, desc);
	spin_unlock_irqrestore(&dmac_chan->lock, flags);
}

static void atcdmac_show_desc(struct atcdmac_chan *dmac_chan,
			      struct atcdmac_desc *desc)
{
	struct device *dev = atcdmac_chan_to_dev(&dmac_chan->dma_chan);

	dev_dbg(dev, "Dump desc info of chan: %u\n", dmac_chan->chan_id);
	dev_dbg(dev, "chan ctrl: 0x%08x\n", desc->regs.ctrl);
	dev_dbg(dev, "trans size: 0x%08x\n", desc->regs.trans_size);
	dev_dbg(dev, "src addr: hi:0x%08x lo:0x%08x\n",
		desc->regs.src_addr_hi,
		desc->regs.src_addr_lo);
	dev_dbg(dev, "dst addr: hi:0x%08x lo:0x%08x\n",
		desc->regs.dst_addr_hi,
		desc->regs.dst_addr_lo);
	dev_dbg(dev, "link addr: hi:0x%08x lo:0x%08x\n",
		desc->regs.ll_ptr_hi,
		desc->regs.ll_ptr_lo);
}

/**
 * atcdmac_chain_desc - Chain a DMA descriptor into a linked-list
 * @first: Pointer to the first descriptor in the chain
 * @prev: Pointer to the previous descriptor in the chain
 * @desc: The descriptor to be added to the chain
 * @cyclic: Indicates if the transfer operates in cyclic mode
 *
 * This function appends a DMA descriptor (desc) to a linked-list of
 * descriptors. If this is the first descriptor being added, it initializes
 * the list and sets *first to point to desc. Otherwise, it links the
 * new descriptor to the end of the list managed by *first.
 *
 * For non-cyclic descriptors, it updates the hardware linked list pointers
 * (ll_ptr_lo and ll_ptr_hi) of the previous descriptor (*prev) to point to
 * the physical address of the new descriptor.
 *
 * Finally, it adds the new descriptor to the list and updates *prev to
 * point to the current descriptor (desc).
 */
static void atcdmac_chain_desc(struct atcdmac_desc **first,
			       struct atcdmac_desc **prev,
			       struct atcdmac_desc *desc,
			       bool cyclic)
{
	if (!(*first)) {
		*first = desc;
		desc->at = &desc->tx_list;
	} else {
		if (!cyclic) {
			(*prev)->regs.ll_ptr_lo =
				lower_32_bits(desc->txd.phys);
			(*prev)->regs.ll_ptr_hi =
				upper_32_bits(desc->txd.phys);
		}
		list_add_tail(&desc->desc_node, &(*first)->tx_list);
	}
	*prev = desc;

	desc->regs.ll_ptr_hi = 0;
	desc->regs.ll_ptr_lo = 0;
}

/**
 * atcdmac_start_transfer - Start the DMA engine with the provided descriptor
 * @dmac_chan: The DMA channel to be started
 * @first_desc: The first descriptor in the list to begin the transfer
 *
 * This function configures the DMA engine by programming the hardware
 * registers with the information from the provided descriptor (first_desc).
 * It then starts the DMA transfer for the specified channel (dmac_chan).
 *
 * The first_desc contains the initial configuration for the transfer,
 * including source and destination addresses, transfer size, and any linked
 * list pointers for subsequent descriptors in the chain.
 */
static void atcdmac_start_transfer(struct atcdmac_chan *dmac_chan,
				   struct atcdmac_desc *first_desc)
{
	struct atcdmac_dmac *dmac = dmac_chan->dma_dev;
	struct regmap *reg = dmac_chan->regmap;
	unsigned long flags;

	spin_lock_irqsave(&dmac->lock, flags);
	dmac->used_chan |= BIT(dmac_chan->chan_id);
	spin_unlock_irqrestore(&dmac->lock, flags);

	regmap_write(reg, REG_CH_CTL_OFF, first_desc->regs.ctrl);
	regmap_write(reg, REG_CH_SIZE_OFF, first_desc->regs.trans_size);
	regmap_write(reg, REG_CH_SRC_LOW_OFF, first_desc->regs.src_addr_lo);
	regmap_write(reg, REG_CH_DST_LOW_OFF, first_desc->regs.dst_addr_lo);
	regmap_write(reg, REG_CH_LLP_LOW_OFF, first_desc->regs.ll_ptr_lo);
	regmap_write(reg, REG_CH_SRC_HIGH_OFF, first_desc->regs.src_addr_hi);
	regmap_write(reg, REG_CH_DST_HIGH_OFF, first_desc->regs.dst_addr_hi);
	regmap_write(reg, REG_CH_LLP_HIGH_OFF, first_desc->regs.ll_ptr_hi);
	atcdmac_enable_chan(dmac_chan, 1);
}

/**
 * atcdmac_start_next_trans - Retrieve and initiate the next DMA transfer
 * @dmac_chan: Pointer to the DMA channel structure
 *
 * In non-cyclic mode, if active_list is empty, the function moves
 * descriptors from queue_list (if available) to active_list and starts
 * the transfer. If both lists are empty, it marks the channel as unused.
 * If there are already active descriptors in active_list, the function
 * retrieves the next DMA descriptor from it and starts the transfer.
 *
 * In cyclic mode, the function retrieves the next DMA descriptor
 * from the linked list of tx_list to maintain continuous transfers.
 */
static void atcdmac_start_next_trans(struct atcdmac_chan *dmac_chan)
{
	struct atcdmac_desc *next_tx = NULL;
	struct atcdmac_desc *dma_desc;

	if (dmac_chan->cyclic) {
		/* Get the next DMA descriptor from tx_list. */
		dma_desc = atcdmac_get_active_head(dmac_chan);
		dma_desc->at = dma_desc->at->next;
		if ((uintptr_t)dma_desc->at == (uintptr_t)&dma_desc->tx_list)
			next_tx = list_entry(dma_desc->at,
					     struct atcdmac_desc,
					     tx_list);
		else
			next_tx = list_entry(dma_desc->at,
					     struct atcdmac_desc,
					     desc_node);
	} else {
		if (list_empty(&dmac_chan->active_list)) {
			if (!list_empty(&dmac_chan->queue_list)) {
				list_splice_init(&dmac_chan->queue_list,
						 &dmac_chan->active_list);
				next_tx = atcdmac_get_active_head(dmac_chan);
			}
		} else {
			next_tx = atcdmac_get_active_head(dmac_chan);
		}
	}

	if (next_tx) {
		dmac_chan->chan_used = 1;
		atcdmac_start_transfer(dmac_chan, next_tx);
	} else {
		dmac_chan->chan_used = 0;
	}
}

static void atcdmac_run_tx_complete_actions(struct atcdmac_desc *desc,
					    enum dmaengine_tx_result result)
{
	struct dma_async_tx_descriptor *txd = &desc->txd;
	struct dmaengine_result res;

	res.result = result;
	dma_cookie_complete(txd);
	dma_descriptor_unmap(txd);
	dmaengine_desc_get_callback_invoke(txd, &res);
	dma_run_dependencies(txd);
}

/**
 * atcdmac_advance_work - Process the completed transaction and move to the
 *                        next descriptor
 * @dmac_chan: DMA channel where the transaction has completed
 *
 * This function is responsible for performing necessary operations after
 * a DMA transaction completes successfully. It retrieves the next descriptor
 * from the active list, initiates its transfer, and communicates the result
 * of the completed transaction to the DMA engine framework.
 */
static void atcdmac_advance_work(struct atcdmac_chan *dmac_chan)
{
	struct atcdmac_dmac *dmac =
		atcdmac_dev_to_dmac(dmac_chan->dma_chan.device);
	struct atcdmac_desc *desc_next, *dma_desc, *desc;
	struct dmaengine_result res;
	LIST_HEAD(completed);
	unsigned long flags;
	unsigned short stop;

	spin_lock_irqsave(&dmac_chan->lock, flags);
	if (list_empty(&dmac_chan->active_list)) {
		spin_unlock_irqrestore(&dmac_chan->lock, flags);
		return;
	}

	dma_desc = atcdmac_get_active_head(dmac_chan);
	stop = READ_ONCE(dmac->stop_mask) & BIT(dmac_chan->chan_id);
	if (dmac_chan->cyclic) {
		if (!stop)
			atcdmac_start_next_trans(dmac_chan);

		spin_unlock_irqrestore(&dmac_chan->lock, flags);
		res.result = DMA_TRANS_NOERROR;
		dmaengine_desc_get_callback_invoke(&dma_desc->txd, &res);
	} else {
		if (list_is_singular(&dmac_chan->active_list)) {
			list_splice_init(&dmac_chan->active_list, &completed);
			list_splice_init(&dmac_chan->queue_list,
					 &dmac_chan->active_list);
		} else {
			list_move_tail(&dma_desc->desc_node, &completed);
		}

		if (!stop)
			atcdmac_start_next_trans(dmac_chan);

		spin_unlock_irqrestore(&dmac_chan->lock, flags);

		list_for_each_entry_safe(desc,
					 desc_next,
					 &completed,
					 desc_node) {
			atcdmac_run_tx_complete_actions(desc,
							DMA_TRANS_NOERROR);
			atcdmac_put_desc(dmac_chan, desc);
		}
	}
}

/**
 * atcdmac_handle_error - Handle errors reported by the DMA controller
 * @dmac_chan: DMA channel where the error occurred
 *
 * This function is invoked when the DMA controller detects an error during a
 * transaction. This function ensures that the DMA channel can recover from
 * errors while reporting issues to aid in debugging. It prevents the DMA
 * controller from operating on potentially invalid memory regions and
 * attempts to maintain system stability.
 */
static void atcdmac_handle_error(struct atcdmac_chan *dmac_chan)
{
	struct device *dev = atcdmac_chan_to_dev(&dmac_chan->dma_chan);
	struct atcdmac_dmac *dmac =
		atcdmac_dev_to_dmac(dmac_chan->dma_chan.device);
	struct atcdmac_desc *bad_desc;
	unsigned long flags;
	unsigned short stop;

	spin_lock_irqsave(&dmac_chan->lock, flags);

	/*
	 * If the active list is empty, the descriptor has already been
	 * handled by another function (e.g., atcdmac_terminate_all()).
	 * Therefore, no further action is needed.
	 */
	if (!list_empty(&dmac_chan->active_list)) {
		/*
		 * Identify the problematic descriptor at the head of the
		 * active list and remove it from the list for further
		 * processing.
		 */
		bad_desc = atcdmac_get_active_head(dmac_chan);
		list_del_init(&bad_desc->desc_node);

		/*
		 * Transfer any pending descriptors from the queue list to the
		 * active list, allowing them to be processed in subsequent
		 * operations.
		 */
		list_splice_init(&dmac_chan->queue_list,
				 dmac_chan->active_list.prev);

		stop = READ_ONCE(dmac->stop_mask) & BIT(dmac_chan->chan_id);
		if (!list_empty(&dmac_chan->active_list) && stop == 0)
			atcdmac_start_transfer(dmac_chan, atcdmac_get_active_head(dmac_chan));
		else
			dmac_chan->chan_used = 0;

		spin_unlock_irqrestore(&dmac_chan->lock, flags);

		/*
		 * Show the details information of the bad descriptor and
		 * return "DMA_TRANS_ABORTED" to the DMA engine framework.
		 */
		dev_err(dev, "DMA transaction failed, possible DMA desc error\n");
		atcdmac_show_desc(dmac_chan, bad_desc);
		atcdmac_run_tx_complete_actions(bad_desc, DMA_TRANS_ABORTED);

		atcdmac_put_desc(dmac_chan, bad_desc);

		return;
	}

	spin_unlock_irqrestore(&dmac_chan->lock, flags);
}

static irqreturn_t atcdmac_irq_thread(int irq, void *dev_id)
{
	struct atcdmac_dmac *dmac = dev_id;
	struct atcdmac_chan *dmac_chan;
	int i;
	bool handled = false;

	for (i = 0; i < dmac->num_ch; i++) {
		dmac_chan = &dmac->chan[i];

		if (test_and_clear_bit(ATCDMAC_STA_TC, &dmac_chan->status)) {
			atcdmac_advance_work(dmac_chan);
			handled = true;
		}

		if (test_and_clear_bit(ATCDMAC_STA_ERR, &dmac_chan->status)) {
			atcdmac_handle_error(dmac_chan);
			handled = true;
		}

		/*
		 * ATCDMAC_STA_ABORT only occurs when the DMA channel is
		 * terminated or freed, and all descriptors and callbacks
		 * have already been processed. Therefore, no additional
		 * handling is required.
		 */
		if (test_and_clear_bit(ATCDMAC_STA_ABORT, &dmac_chan->status))
			handled = true;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static irqreturn_t atcdmac_interrupt(int irq, void *dev_id)
{
	struct atcdmac_dmac *dmac = dev_id;
	struct atcdmac_chan *dmac_chan;
	unsigned int status;
	unsigned int int_ch;
	int ret = IRQ_NONE;
	int i;

	regmap_read(dmac->regmap, REG_INT_STA, &status);
	int_ch = READ_ONCE(dmac->used_chan) & DMA_INT_ALL(status);

	while (int_ch) {
		spin_lock(&dmac->lock);
		dmac->used_chan = READ_ONCE(dmac->used_chan) & ~int_ch;
		spin_unlock(&dmac->lock);
		regmap_write(dmac->regmap, REG_INT_STA, DMA_INT_CLR(int_ch));

		for (i = 0; i < dmac->num_ch; i++) {
			if (int_ch & BIT(i)) {
				int_ch &= ~BIT(i);
				dmac_chan = &dmac->chan[i];

				if (status & DMA_TC(i))
					set_bit(ATCDMAC_STA_TC,
						&dmac_chan->status);

				if (status & DMA_ERR(i))
					set_bit(ATCDMAC_STA_ERR,
						&dmac_chan->status);

				if (status & DMA_ABT(i))
					set_bit(ATCDMAC_STA_ABORT,
						&dmac_chan->status);

				ret = IRQ_WAKE_THREAD;
			}
			if (!int_ch)
				break;
		}

		regmap_read(dmac->regmap, REG_INT_STA, &status);
		int_ch = READ_ONCE(dmac->used_chan) & DMA_INT_ALL(status);
	}

	return ret;
}

/**
 * atcdmac_issue_pending - Trigger the execution of queued DMA transactions
 * @chan: DMA channel on which to issue pending transactions
 *
 * This function checks if the DMA channel is currently idle and if there are
 * descriptors waiting in the queue_list. If the channel is idle and the
 * queue_list is not empty, it moves the first queued descriptor to the active
 * list and starts the DMA transfer by calling atcdmac_start_transfer().
 */
static void atcdmac_issue_pending(struct dma_chan *chan)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_dmac *dmac =
		atcdmac_dev_to_dmac(dmac_chan->dma_chan.device);
	unsigned long flags;
	unsigned short stop;

	spin_lock_irqsave(&dmac_chan->lock, flags);
	stop = READ_ONCE(dmac->stop_mask) & BIT(dmac_chan->chan_id);
	if (dmac_chan->chan_used == 0 && stop == 0 &&
	    !list_empty(&dmac_chan->queue_list)) {
		dmac_chan->chan_used = 1;
		list_move(dmac_chan->queue_list.next, &dmac_chan->active_list);
		atcdmac_start_transfer(dmac_chan,
				       atcdmac_get_active_head(dmac_chan));
	}

	spin_unlock_irqrestore(&dmac_chan->lock, flags);
}

static unsigned char atcdmac_map_buswidth(enum dma_slave_buswidth addr_width)
{
	switch (addr_width) {
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
		return 0;
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
		return 1;
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		return 2;
	case DMA_SLAVE_BUSWIDTH_8_BYTES:
		return 3;
	case DMA_SLAVE_BUSWIDTH_16_BYTES:
		return 4;
	case DMA_SLAVE_BUSWIDTH_32_BYTES:
		return 5;
	default:
		return 0;
	}
}

static unsigned int atcdmac_map_tran_width(dma_addr_t src,
					   dma_addr_t dst,
					   size_t len,
					   unsigned int max_align_bytes)
{
	unsigned int align = src | dst | len | max_align_bytes;
	unsigned int width;

	if (!(align & 0x1F))
		width = WIDTH_32_BYTES;
	else if (!(align & 0xF))
		width = WIDTH_16_BYTES;
	else if (!(align & 0x7))
		width = WIDTH_8_BYTES;
	else if (!(align & 0x3))
		width = WIDTH_4_BYTES;
	else if (!(align & 0x1))
		width = WIDTH_2_BYTES;
	else
		width = WIDTH_1_BYTE;

	return width;
}

/**
 * atcdmac_convert_burst - Convert burst size to a power of two index
 * @burst_size: Actual burst size in bytes
 *
 * This function converts a burst size (e.g., 1, 2, 4, 8...) into the
 * corresponding hardware-encoded value, which represents the index of
 * the power of two.
 *
 * Return: The zero-based power-of-two index corresponding to @burst_size.
 *
 * Example:
 * If burst_size is 8 (binary 1000), the most significant bit is at position
 * 4, so the function returns 3 (i.e., 4 - 1).
 */
static unsigned char atcdmac_convert_burst(unsigned int burst_size)
{
	return fls(burst_size) - 1;
}

static struct atcdmac_desc *
atcdmac_build_desc(struct atcdmac_chan *dmac_chan,
		   dma_addr_t src,
		   dma_addr_t dst,
		   unsigned int ctrl,
		   unsigned int trans_size,
		   unsigned int num_sg)
{
	struct atcdmac_desc *desc;

	desc = atcdmac_get_desc(dmac_chan);
	if (!desc)
		return NULL;

	desc->regs.src_addr_lo = lower_32_bits(src);
	desc->regs.src_addr_hi = upper_32_bits(src);
	desc->regs.dst_addr_lo = lower_32_bits(dst);
	desc->regs.dst_addr_hi = upper_32_bits(dst);
	desc->regs.ctrl = ctrl;
	desc->regs.trans_size = trans_size;
	desc->num_sg = num_sg;

	return desc;
}

/**
 * atcdmac_prep_dma_memcpy - Prepare a DMA memcpy operation for the specified
 *                           channel
 * @chan: DMA channel to configure for the operation
 * @dst: Physical destination address for the transfer
 * @src: Physical source address for the transfer
 * @len: Size of the data to transfer, in bytes
 * @flags: Status flags for the transfer descriptor
 *
 * This function sets up a DMA memcpy operation to transfer data from the
 * specified source address to the destination address. It returns a DMA
 * descriptor that represents the configured transaction.
 */
static struct dma_async_tx_descriptor *
atcdmac_prep_dma_memcpy(struct dma_chan *chan,
			dma_addr_t dst,
			dma_addr_t src,
			size_t len,
			unsigned long flags)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_dmac *dmac = atcdmac_dev_to_dmac(chan->device);
	struct atcdmac_desc *desc;
	unsigned int src_width;
	unsigned int dst_width;
	unsigned int ctrl;
	unsigned char src_max_burst;

	if (unlikely(!len)) {
		dev_warn(atcdmac_chan_to_dev(chan),
			 "Failed to prepare DMA operation: len is zero\n");
		return NULL;
	}

	src_max_burst =
		atcdmac_convert_burst((unsigned int)SRC_BURST_SIZE_1024);
	src_width = atcdmac_map_tran_width(src,
					   dst,
					   len,
					   1 << dmac->data_width);
	dst_width = src_width;
	ctrl = SRC_BURST_SIZE(src_max_burst) |
	       SRC_ADDR_MODE_INCR |
	       DST_ADDR_MODE_INCR |
	       DST_WIDTH(dst_width) |
	       SRC_WIDTH(src_width);

	desc = atcdmac_build_desc(dmac_chan, src, dst, ctrl,
				  len >> src_width, 1);
	if (!desc)
		goto err_desc_get;

	return &desc->txd;

err_desc_get:
	dev_warn(atcdmac_chan_to_dev(chan), "Failed to allocate descriptor\n");
	return NULL;
}

static int atcdmac_get_slave_cfg(struct dma_slave_config *sconfig,
				 enum dma_transfer_direction dir,
				 struct atcdmac_slave_cfg *cfg)
{
	if (dir == DMA_MEM_TO_DEV) {
		cfg->reg         = sconfig->dst_addr;
		cfg->burst_bytes = sconfig->dst_addr_width * sconfig->dst_maxburst;
		cfg->dev_width   = sconfig->dst_addr_width;
		cfg->width_src   = atcdmac_map_buswidth(sconfig->src_addr_width);
		cfg->width_dst   = atcdmac_map_buswidth(sconfig->dst_addr_width);
	} else if (dir == DMA_DEV_TO_MEM) {
		cfg->reg         = sconfig->src_addr;
		cfg->burst_bytes = sconfig->src_addr_width * sconfig->src_maxburst;
		cfg->dev_width   = sconfig->src_addr_width;
		cfg->width_src   = atcdmac_map_buswidth(sconfig->src_addr_width);
		cfg->width_dst   = atcdmac_map_buswidth(sconfig->dst_addr_width);
	} else {
		return -EINVAL;
	}
	return 0;
}

static struct atcdmac_desc *
atcdmac_build_slave_desc(struct atcdmac_chan *dmac_chan,
			 const struct atcdmac_slave_cfg *cfg,
			 dma_addr_t mem, unsigned int len,
			 enum dma_transfer_direction dir,
			 unsigned int data_width, unsigned int num_desc)
{
	unsigned int   width_cal;
	unsigned short burst_size;
	unsigned int   ctrl;
	dma_addr_t     src, dst;

	width_cal = atcdmac_map_tran_width(mem, cfg->reg, len,
					   (1 << data_width) | cfg->burst_bytes);
	if (dir == DMA_MEM_TO_DEV) {
		if (cfg->burst_bytes < (1 << width_cal)) {
			burst_size = cfg->burst_bytes;
			width_cal  = WIDTH_1_BYTE;
		} else {
			burst_size = cfg->burst_bytes / (1 << width_cal);
		}
		ctrl = SRC_ADDR_MODE_INCR | DST_ADDR_MODE_FIXED |
		       DST_HS | DST_REQ(dmac_chan->req_num) |
		       SRC_WIDTH(width_cal) | DST_WIDTH(cfg->width_dst) |
		       SRC_BURST_SIZE(ilog2(burst_size));
		src = mem;
		dst = cfg->reg;
	} else {
		burst_size = cfg->burst_bytes / cfg->dev_width;
		ctrl = SRC_ADDR_MODE_FIXED | DST_ADDR_MODE_INCR |
		       SRC_HS | SRC_REQ(dmac_chan->req_num) |
		       SRC_WIDTH(cfg->width_src) | DST_WIDTH(width_cal) |
		       SRC_BURST_SIZE(ilog2(burst_size));
		src      = cfg->reg;
		dst      = mem;
		width_cal = cfg->width_src;
	}
	return atcdmac_build_desc(dmac_chan, src, dst, ctrl,
				  len >> width_cal, num_desc);
}

/**
 * atcdmac_prep_device_sg - Prepare descriptors for memory/device DMA
 *                          transactions
 * @chan: DMA channel to configure for the operation
 * @sgl: Scatter-gather list representing the memory regions to transfer
 * @sg_len: Number of entries in the scatter-gather list
 * @direction: Direction of the DMA transfer
 * @flags: Status flags for the transfer descriptor
 * @context: transaction context (ignored)
 *
 * This function prepares a DMA transaction by setting up the required
 * descriptors based on the provided scatter-gather list and parameters.
 * It supports memory-to-device and device-to-memory DMA transfers.
 */
static struct dma_async_tx_descriptor *
atcdmac_prep_device_sg(struct dma_chan *chan,
		       struct scatterlist *sgl,
		       unsigned int sg_len,
		       enum dma_transfer_direction direction,
		       unsigned long flags,
		       void *context)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_dmac *dmac = atcdmac_dev_to_dmac(chan->device);
	struct atcdmac_slave_cfg cfg;
	struct atcdmac_desc *first = NULL;
	struct atcdmac_desc *prev = NULL;
	struct scatterlist *sg;
	unsigned int i;

	if (unlikely(!sg_len)) {
		dev_warn(atcdmac_chan_to_dev(chan), "sg_len is zero\n");
		return NULL;
	}

	if (atcdmac_get_slave_cfg(&dmac_chan->dma_sconfig, direction, &cfg)) {
		dev_err(atcdmac_chan_to_dev(chan),
			"Invalid transfer direction %d\n", direction);
		return NULL;
	}

	for_each_sg(sgl, sg, sg_len, i) {
		struct atcdmac_desc *desc;
		dma_addr_t mem = sg_dma_address(sg);
		unsigned int len = sg_dma_len(sg);

		if (unlikely(!len)) {
			dev_err(atcdmac_chan_to_dev(chan),
				"sg(%u) data len is zero\n", i);
			goto err;
		}

		desc = atcdmac_build_slave_desc(dmac_chan, &cfg, mem, len,
						direction, dmac->data_width,
						sg_len);
		if (!desc)
			goto err_desc_get;

		atcdmac_chain_desc(&first, &prev, desc, false);
	}

	first->txd.cookie = -EBUSY;
	first->txd.flags = flags;

	return &first->txd;

err_desc_get:
	dev_warn(atcdmac_chan_to_dev(chan), "Failed to allocate descriptor\n");
	if (first)
		first->num_sg = i;

err:
	if (first)
		atcdmac_put_desc(dmac_chan, first);
	return NULL;
}

static struct dma_async_tx_descriptor *
atcdmac_prep_dma_cyclic(struct dma_chan *chan,
			dma_addr_t buf_addr,
			size_t buf_len,
			size_t period_len,
			enum dma_transfer_direction direction,
			unsigned long flags)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_dmac *dmac = atcdmac_dev_to_dmac(chan->device);
	struct atcdmac_slave_cfg cfg;
	struct atcdmac_desc *first = NULL;
	struct atcdmac_desc *prev = NULL;
	unsigned int num_periods;
	unsigned int period;

	if (period_len == 0 || buf_len == 0) {
		dev_warn(atcdmac_chan_to_dev(chan),
			 "invalid cyclic params buf_len=%zu period_len=%zu\n",
			 buf_len, period_len);
		return NULL;
	}

	if (atcdmac_get_slave_cfg(&dmac_chan->dma_sconfig, direction, &cfg)) {
		dev_err(atcdmac_chan_to_dev(chan),
			"Invalid transfer direction %d\n", direction);
		return NULL;
	}

	num_periods = (buf_len + period_len - 1) / period_len;

	for (period = 0; period < buf_len; period += period_len) {
		struct atcdmac_desc *desc;
		dma_addr_t mem = buf_addr + period;
		unsigned int len = min_t(unsigned int, period_len,
					 buf_len - period);

		desc = atcdmac_build_slave_desc(dmac_chan, &cfg, mem, len,
						direction, dmac->data_width,
						num_periods);
		if (!desc)
			goto err_desc_get;
		atcdmac_chain_desc(&first, &prev, desc, true);
	}

	first->txd.flags = flags;
	dmac_chan->cyclic = true;

	return &first->txd;

err_desc_get:
	dev_warn(atcdmac_chan_to_dev(chan), "Failed to allocate descriptor\n");
	if (first)
		atcdmac_put_desc(dmac_chan, first);

	return NULL;
}

static int atcdmac_set_device_config(struct dma_chan *chan,
				     struct dma_slave_config *sconfig)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);

	/* Check if this chan is configured for device transfers */
	if (!dmac_chan->dev_chan)
		return -EINVAL;

	/* Must be powers of two according to ATCDMAC300 spec */
	if (!is_power_of_2(sconfig->src_maxburst) ||
	    !is_power_of_2(sconfig->dst_maxburst) ||
	    !is_power_of_2(sconfig->src_addr_width) ||
	    !is_power_of_2(sconfig->dst_addr_width))
		return -EINVAL;

	memcpy(&dmac_chan->dma_sconfig, sconfig, sizeof(*sconfig));

	return 0;
}

static int atcdmac_terminate_all(struct dma_chan *chan)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_desc *desc_cur, *desc_next;
	LIST_HEAD(list);
	unsigned long flags;
	unsigned int val;
	int ret;

	spin_lock_irqsave(&dmac_chan->lock, flags);
	atcdmac_abort_chan(dmac_chan);
	atcdmac_enable_chan(dmac_chan, 0);
	ret = regmap_read_poll_timeout_atomic(dmac_chan->dma_dev->regmap,
					      REG_CH_EN,
					      val,
					      !(val & BIT(dmac_chan->chan_id)),
					      10,
					      ATCDMAC_CHAN_TIMEOUT_US);
	if (ret)
		dev_err(atcdmac_chan_to_dev(chan),
			"Timed out waiting for channel to disable\n");

	list_splice_init(&dmac_chan->queue_list,  &list);
	list_splice_init(&dmac_chan->active_list, &list);
	dmac_chan->chan_used = 0;
	spin_unlock_irqrestore(&dmac_chan->lock, flags);

	list_for_each_entry_safe(desc_cur, desc_next, &list, desc_node) {
		atcdmac_run_tx_complete_actions(desc_cur, DMA_TRANS_ABORTED);
		atcdmac_put_desc(dmac_chan, desc_cur);
	}

	return ret;
}

static enum dma_status atcdmac_get_tx_status(struct dma_chan *chan,
					     dma_cookie_t cookie,
					     struct dma_tx_state *state)
{
	return dma_cookie_status(chan, cookie, state);
}

static int atcdmac_wait_chan_idle(struct atcdmac_dmac *dmac,
				  unsigned short chan_mask,
				  unsigned int timeout_us)
{
	unsigned int val;
	int ret;

	ret = regmap_read_poll_timeout(dmac->regmap,
				       REG_CH_EN,
				       val,
				       !(val & chan_mask),
				       50,
				       timeout_us);
	if (ret)
		dev_err(dmac->dma_device.dev,
			"Timeout waiting for device ready %d\n", ret);

	return ret;
}

/**
 * atcdmac_alloc_chan_resources - Allocate resources for a DMA channel
 * @chan: The DMA channel for which resources are being allocated
 *
 * This function sets up and allocates the necessary resources for the
 * specified DMA channel (chan). It ensures the channel is prepared to
 * handle DMA requests from clients by allocating descriptors and any other
 * required resources.
 *
 * Return: The number of descriptors successfully allocated, or a negative
 *         error code on failure.
 */
static int atcdmac_alloc_chan_resources(struct dma_chan *chan)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_dmac *dmac = atcdmac_dev_to_dmac(chan->device);
	struct atcdmac_desc *desc;
	int i;

	if (atcdmac_is_chan_enable(dmac_chan)) {
		dev_err(atcdmac_chan_to_dev(chan),
			"DMA channel is not in an idle state\n");
		return -EBUSY;
	}

	if (!list_empty(&dmac_chan->free_list))
		return dmac_chan->descs_allocated;

	/*
	 * Spin-lock protection is not necessary during DMA channel
	 * initialization, as the channel is not yet in use at this stage,
	 * and the shared resources are not accessed by other threads.
	 */
	for (i = 0; i < ATCDMAC_DESC_PER_CHAN; i++) {
		desc = atcdmac_alloc_desc(chan, GFP_KERNEL);
		if (!desc) {
			dev_warn(dmac->dma_device.dev,
				 "Insufficient descriptors: only %d descriptors available\n",
				 i);
			break;
		}
		list_add_tail(&desc->desc_node, &dmac_chan->free_list);
	}
	dmac_chan->descs_allocated = i;
	dmac_chan->cyclic = false;
	dma_cookie_init(chan);
	spin_lock_irq(&dmac->lock);
	dmac->stop_mask &= ~BIT(dmac_chan->chan_id);
	spin_unlock_irq(&dmac->lock);

	return dmac_chan->descs_allocated;
}

/**
 * atcdmac_free_chan_resources - Release a DMA channel's resources
 * @chan: The DMA channel to release
 *
 * This function ensures that any remaining DMA descriptors in the
 * active_list and queue_list are properly reclaimed. It releases all
 * resources associated with the specified DMA channel and resets the
 * channel's management structures to their initial states.
 */
static void atcdmac_free_chan_resources(struct dma_chan *chan)
{
	struct atcdmac_chan *dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	struct atcdmac_dmac *dmac = atcdmac_dev_to_dmac(chan->device);
	struct atcdmac_desc *desc_next, *desc;
	unsigned long flags;

	WARN_ON_ONCE(atcdmac_is_chan_enable(dmac_chan));

	spin_lock_irq(&dmac->lock);
	dmac->stop_mask |= BIT(dmac_chan->chan_id);
	spin_unlock_irq(&dmac->lock);

	atcdmac_terminate_all(chan);

	spin_lock_irqsave(&dmac_chan->lock, flags);
	list_for_each_entry_safe(desc,
				 desc_next,
				 &dmac_chan->free_list,
				 desc_node) {
		list_del(&desc->desc_node);
		dma_pool_free(dmac->dma_desc_pool, desc, desc->txd.phys);
	}

	INIT_LIST_HEAD(&dmac_chan->free_list);
	dmac_chan->descs_allocated = 0;
	dmac_chan->status = 0;
	dmac_chan->chan_used = 0;
	dmac_chan->dev_chan = 0;
	spin_unlock_irqrestore(&dmac_chan->lock, flags);
}

static bool atcdmac_filter_chan(struct dma_chan *chan, void *dma_dev)
{
	if (dma_dev == chan->device->dev)
		return true;

	return false;
}

static struct dma_chan *atcdmac_dma_xlate_handler(struct of_phandle_args *dmac,
						  struct of_dma *of_dma)
{
	struct platform_device *dmac_pdev;
	struct atcdmac_chan *dmac_chan;
	struct dma_chan *chan;
	dma_cap_mask_t mask;

	dmac_pdev = of_find_device_by_node(dmac->np);
	if (!dmac_pdev)
		return NULL;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	chan = dma_request_channel(mask, atcdmac_filter_chan, &dmac_pdev->dev);
	put_device(&dmac_pdev->dev);

	if (!chan)
		return NULL;

	dmac_chan = atcdmac_chan_to_dmac_chan(chan);
	dmac_chan->dev_chan = true;
	dmac_chan->req_num = dmac->args[0] & 0xff;

	return chan;
}

/**
 * atcdmac_reset_and_wait_chan_idle - Reset the DMA controller and wait for
 *                                    all channels to become idle
 * @dmac: Pointer to the DMA controller structure
 *
 * This function performs a reset of the DMA controller and ensures that all
 * DMA channels are disabled.
 */
static int atcdmac_reset_and_wait_chan_idle(struct atcdmac_dmac *dmac)
{
	regmap_update_bits(dmac->regmap, REG_CTL, DMAC_RESET, 1);
	msleep(20);
	regmap_update_bits(dmac->regmap, REG_CTL, DMAC_RESET, 0);

	return atcdmac_wait_chan_idle(dmac,
				      BIT(dmac->num_ch) - 1,
				      ATCDMAC_CHAN_TIMEOUT_US);
}

static int atcdmac_restore_iocp(struct atcdmac_dmac *dmac)
{
	if (!dmac->regmap_iocp)
		return 0;

	return regmap_write(dmac->regmap_iocp,
			    0,
			    IOCP_CACHE_DMAC0_AW |
			    IOCP_CACHE_DMAC0_AR |
			    IOCP_CACHE_DMAC1_AW |
			    IOCP_CACHE_DMAC1_AR);
}

static int atcdmac_init_iocp(struct platform_device *pdev,
			     struct atcdmac_dmac *dmac)
{
	const struct regmap_config iocp_regmap_config = {
		.name = "iocp",
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.pad_bits = 0,
		.max_register = 0,
		.cache_type = REGCACHE_NONE,
	};
	struct resource *res;
	void __iomem *regs;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iocp");
	if (!res) {
		dmac->regmap_iocp = NULL;
		return 0;
	}

	regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(regs)) {
		dmac->regmap_iocp = NULL;
		return dev_err_probe(&pdev->dev, PTR_ERR(regs),
				     "Failed to create ioremap for IOCP\n");
	}

	dmac->regmap_iocp = devm_regmap_init_mmio(&pdev->dev,
						  regs,
						  &iocp_regmap_config);
	if (IS_ERR(dmac->regmap_iocp)) {
		int ret = PTR_ERR(dmac->regmap_iocp);

		dmac->regmap_iocp = NULL;
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to create regmap for IOCP\n");
	}

	return atcdmac_restore_iocp(dmac);
}

static void atcdmac_init_dma_device(struct platform_device *pdev,
				    struct atcdmac_dmac *dmac)
{
	struct dma_device *device = &dmac->dma_device;

	device->device_alloc_chan_resources = atcdmac_alloc_chan_resources;
	device->device_free_chan_resources = atcdmac_free_chan_resources;
	device->device_tx_status = atcdmac_get_tx_status;
	device->device_issue_pending = atcdmac_issue_pending;
	device->device_prep_dma_memcpy = atcdmac_prep_dma_memcpy;
	device->device_prep_slave_sg = atcdmac_prep_device_sg;
	device->device_config = atcdmac_set_device_config;
	device->device_terminate_all = atcdmac_terminate_all;
	device->device_prep_dma_cyclic = atcdmac_prep_dma_cyclic;

	device->dev = &pdev->dev;
	device->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
				  BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				  BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	device->dst_addr_widths = device->src_addr_widths;
	device->directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	device->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;

	dma_cap_set(DMA_SLAVE, device->cap_mask);
	dma_cap_set(DMA_MEMCPY, device->cap_mask);
	dma_cap_set(DMA_CYCLIC, device->cap_mask);
}

static int atcdmac_init_channels(struct platform_device *pdev,
				 struct atcdmac_dmac *dmac)
{
	struct regmap_config chan_regmap_config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.pad_bits = 0,
		.cache_type = REGCACHE_NONE,
		.max_register = REG_CH_LLP_HIGH_OFF,
	};
	struct atcdmac_chan *dmac_chan;
	int ret;
	int i;

	INIT_LIST_HEAD(&dmac->dma_device.channels);

	for (i = 0; i < dmac->num_ch; i++) {
		char *regmap_name = kasprintf(GFP_KERNEL, "chan%d", i);

		if (!regmap_name)
			return -ENOMEM;

		dmac_chan = &dmac->chan[i];
		chan_regmap_config.name = regmap_name;
		dmac_chan->regmap =
			devm_regmap_init_mmio(&pdev->dev,
					      dmac->regs + REG_CH_OFF(i),
					      &chan_regmap_config);
		kfree(regmap_name);

		if (IS_ERR(dmac_chan->regmap)) {
			ret = PTR_ERR(dmac_chan->regmap);
			dev_err_probe(&pdev->dev, ret,
				      "Failed to create regmap for DMA chan%d\n",
				      i);
			return ret;
		}

		spin_lock_init(&dmac_chan->lock);
		dmac_chan->dma_chan.device = &dmac->dma_device;
		dmac_chan->dma_dev = dmac;
		dmac_chan->chan_id = i;
		dmac_chan->chan_used = 0;

		INIT_LIST_HEAD(&dmac_chan->active_list);
		INIT_LIST_HEAD(&dmac_chan->queue_list);
		INIT_LIST_HEAD(&dmac_chan->free_list);

		list_add_tail(&dmac_chan->dma_chan.device_node,
			      &dmac->dma_device.channels);
		dma_cookie_init(&dmac_chan->dma_chan);
	}

	return 0;
}

static int atcdmac_init_desc_pool(struct platform_device *pdev,
				  struct atcdmac_dmac *dmac)
{
	dmac->dma_desc_pool = dmam_pool_create(dev_name(&pdev->dev),
					       &pdev->dev,
					       sizeof(struct atcdmac_desc),
					       64,
					       4096);
	if (!dmac->dma_desc_pool)
		return dev_err_probe(&pdev->dev, -ENOMEM,
				     "Failed to create memory pool for DMA descriptors\n");
	return 0;
}

static int atcdmac_init_irq(struct platform_device *pdev,
			    struct atcdmac_dmac *dmac)
{
	int irq = platform_get_irq(pdev, 0);

	if (irq < 0)
		return irq;

	return devm_request_threaded_irq(&pdev->dev,
					 irq,
					 atcdmac_interrupt,
					 atcdmac_irq_thread,
					 IRQF_SHARED | IRQF_ONESHOT,
					 dev_name(&pdev->dev),
					 dmac);
}

static int atcdmac_init_ioremap_and_regmap(struct platform_device *pdev,
					   struct atcdmac_dmac **out_dmac)
{
	const struct regmap_config dmac_regmap_config = {
		.name = dev_name(&pdev->dev),
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.pad_bits = 0,
		.cache_type = REGCACHE_NONE,
		.max_register = REG_CH_EN,
	};
	struct atcdmac_dmac *dmac;
	struct regmap *regmap;
	void __iomem *regs;
	size_t size;
	unsigned int val;
	int ret = 0;
	unsigned char num_ch;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return dev_err_probe(&pdev->dev, PTR_ERR(regs),
				     "Failed to ioremap I/O resource\n");

	regmap = devm_regmap_init_mmio(&pdev->dev, regs, &dmac_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(&pdev->dev, PTR_ERR(regmap),
				     "Failed to create regmap for I/O\n");

	regmap_read(regmap, REG_CFG, &val);
	num_ch = val & CH_NUM;
	size = sizeof(*dmac) + num_ch * sizeof(struct atcdmac_chan);
	dmac = devm_kzalloc(&pdev->dev, size, GFP_KERNEL);
	if (!dmac)
		return -ENOMEM;

	dmac->regmap = regmap;
	dmac->num_ch = num_ch;
	dmac->regs = regs;

	/*
	 * Adjust the AXI bus data width (from the DMAC Configuration
	 * Register) to align with the transfer width encoding (in the
	 * Channel n Control Register). For example, an AXI width of 0
	 * (32-bit) corresponds to a transfer width of 2 (word transfer).
	 */
	dmac->data_width = FIELD_GET(DATA_WIDTH, val) + 2;
	spin_lock_init(&dmac->lock);

	platform_set_drvdata(pdev, dmac);
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64)))
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to set DMA mask\n");

	*out_dmac = dmac;

	return ret;
}

static int atcdmac_probe(struct platform_device *pdev)
{
	struct atcdmac_dmac *dmac;
	int ret;

	ret = atcdmac_init_ioremap_and_regmap(pdev, &dmac);
	if (ret)
		return ret;

	ret = atcdmac_reset_and_wait_chan_idle(dmac);
	if (ret)
		return ret;

	ret = atcdmac_init_desc_pool(pdev, dmac);
	if (ret)
		return ret;

	ret = atcdmac_init_channels(pdev, dmac);
	if (ret)
		return ret;

	atcdmac_init_dma_device(pdev, dmac);

	ret = dma_async_device_register(&dmac->dma_device);
	if (ret)
		return ret;

	ret = atcdmac_init_irq(pdev, dmac);
	if (ret)
		goto err_dma_async_register;

	ret = of_dma_controller_register(pdev->dev.of_node,
					 atcdmac_dma_xlate_handler,
					 dmac);
	if (ret)
		goto err_dma_async_register;

	ret = atcdmac_init_iocp(pdev, dmac);
	if (ret)
		goto err_of_dma_register;

	return 0;

err_of_dma_register:
	of_dma_controller_free(pdev->dev.of_node);
err_dma_async_register:
	dma_async_device_unregister(&dmac->dma_device);

	return ret;
}

static int atcdmac_resume(struct device *dev)
{
	struct dma_chan *chan;
	struct dma_chan *chan_next;
	struct atcdmac_dmac *dmac = dev_get_drvdata(dev);
	struct atcdmac_chan *dmac_chan;
	unsigned long flags;
	int ret;

	ret = atcdmac_reset_and_wait_chan_idle(dmac);
	if (ret)
		return ret;

	ret = atcdmac_restore_iocp(dmac);
	if (ret)
		return ret;

	spin_lock_irqsave(&dmac->lock, flags);
	dmac->stop_mask = 0;
	spin_unlock_irqrestore(&dmac->lock, flags);
	list_for_each_entry_safe(chan,
				 chan_next,
				 &dmac->dma_device.channels,
				 device_node) {
		dmac_chan = atcdmac_chan_to_dmac_chan(chan);
		spin_lock_irqsave(&dmac_chan->lock, flags);
		atcdmac_start_next_trans(dmac_chan);
		spin_unlock_irqrestore(&dmac_chan->lock, flags);
	}

	return 0;
}

static int atcdmac_suspend(struct device *dev)
{
	struct atcdmac_dmac *dmac = dev_get_drvdata(dev);
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&dmac->lock, flags);
	dmac->stop_mask = BIT(dmac->num_ch) - 1;
	spin_unlock_irqrestore(&dmac->lock, flags);
	ret = atcdmac_wait_chan_idle(dmac,
				     dmac->stop_mask,
				     ATCDMAC_CHAN_TIMEOUT_US * dmac->num_ch);

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(atcdmac_pm_ops,
				atcdmac_suspend,
				atcdmac_resume);

static const struct of_device_id atcdmac_dt_ids[] = {
	{ .compatible = "andestech,ae350-dma", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, atcdmac_dt_ids);

static struct platform_driver atcdmac_driver = {
	.probe = atcdmac_probe,
	.driver = {
		.name = "atcdmac300",
		.of_match_table = atcdmac_dt_ids,
		.pm = pm_sleep_ptr(&atcdmac_pm_ops),
	},
};
builtin_platform_driver(atcdmac_driver);
