// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDXI dmaengine provider
 *
 * Copyright Advanced Micro Devices, Inc.
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/container_of.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/spinlock.h>

#include "../dmaengine.h"
#include "../virt-dma.h"
#include "completion.h"
#include "context.h"
#include "descriptor.h"
#include "dma.h"
#include "ring.h"
#include "sdxi.h"

static unsigned short dma_channels = 1;
module_param(dma_channels, ushort, 0644);
MODULE_PARM_DESC(dma_channels, "DMA channels per function (default: 1)");

/*
 * An SDXI context is allocated for each channel configured.
 *
 * Each context has a descriptor ring with a minimum of 1K entries.
 * SDXI supports a variety of primitive operations, e.g. copy,
 * interrupt, nop. Each Linux virtual DMA descriptor may be composed
 * of a grouping of SDXI descriptors in the ring. E.g. two SDXI
 * descriptors (copy, then interrupt) to implement a
 * dma_async_tx_descriptor for memcpy with DMA_PREP_INTERRUPT flag.
 *
 * dma_device->device_prep_dma_* functions reserve space in the
 * descriptor ring and serialize SDXI descriptors implementing the
 * operation to the reserved slots, leaving their valid (vl) bits
 * clear. A single virtual descriptor is added to the allocated list.
 *
 * dma_async_tx_descriptor->tx_submit() invokes vchan_tx_submit(),
 * which merely assigns a cookie and moves the txd to the submitted
 * list without entering the SDXI provider code.
 *
 * dma_device->device_issue_pending() (sdxi_dma_issue_pending()) sets vl
 * on each SDXI descriptor reachable from the submitted list, then
 * rings the context doorbell. The submitted txds are moved to the
 * issued list via vchan_issue_pending().
 */

struct sdxi_dma_chan {
	struct virt_dma_chan vchan;
	struct sdxi_cxt *cxt;
	unsigned int vector;
	unsigned int irq;
	struct sdxi_akey_ent *akey;
};

struct sdxi_dma_dev {
	struct dma_device dma_dev;
	size_t nr_channels;
	struct sdxi_dma_chan sdchan[] __counted_by(nr_channels);
};

/*
 * A virtual descriptor can correspond to a group of SDXI hardware descriptors.
 */
struct sdxi_dma_desc {
	struct virt_dma_desc vdesc;
	struct sdxi_ring_resv resv;
	struct sdxi_completion *completion;
};

static struct sdxi_dma_chan *to_sdxi_dma_chan(const struct dma_chan *dma_chan)
{
	const struct virt_dma_chan *vchan;

	vchan = container_of_const(dma_chan, struct virt_dma_chan, chan);
	return container_of(vchan, struct sdxi_dma_chan, vchan);
}

static struct sdxi_dma_desc *
to_sdxi_dma_desc(const struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct sdxi_dma_desc, vdesc);
}

static void sdxi_tx_desc_free(struct virt_dma_desc *vdesc)
{
	struct sdxi_dma_desc *sddesc = to_sdxi_dma_desc(vdesc);

	sdxi_completion_free(sddesc->completion);
	kfree(to_sdxi_dma_desc(vdesc));
}

static struct sdxi_dma_desc *
prep_memcpy_intr(struct dma_chan *dma_chan, const struct sdxi_copy *params)
{
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	struct sdxi_akey_ent *akey = to_sdxi_dma_chan(dma_chan)->akey;
	struct sdxi_desc *copy, *intr;

	struct sdxi_completion *comp __free(sdxi_completion) = sdxi_completion_alloc(cxt->sdxi);
	if (!comp)
		return NULL;

	struct sdxi_dma_desc *sddesc __free(kfree) = kzalloc(sizeof(*sddesc), GFP_NOWAIT);
	if (!sddesc)
		return NULL;

	if (sdxi_ring_try_reserve(cxt->ring_state, 2, &sddesc->resv))
		return NULL;

	copy = sdxi_ring_resv_next(&sddesc->resv);
	(void)sdxi_encode_copy(copy, params); /* Caller checked validity. */
	sdxi_desc_set_fence(copy); /* Conservatively fence every descriptor. */
	sdxi_completion_attach(copy, comp);

	sddesc->completion = no_free_ptr(comp);

	intr = sdxi_ring_resv_next(&sddesc->resv);
	sdxi_encode_intr(intr, &(const struct sdxi_intr) {
			.akey = sdxi_akey_index(cxt, akey),
		});
	/* Raise the interrupt only after the copy has completed. */
	sdxi_desc_set_fence(intr);
	return_ptr(sddesc);
}

static struct sdxi_dma_desc *
prep_memcpy_polled(struct dma_chan *dma_chan, const struct sdxi_copy *params)
{
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	struct sdxi_desc *copy;

	struct sdxi_completion *comp __free(sdxi_completion) = sdxi_completion_alloc(cxt->sdxi);
	if (!comp)
		return NULL;

	struct sdxi_dma_desc *sddesc __free(kfree) = kzalloc(sizeof(*sddesc), GFP_NOWAIT);
	if (!sddesc)
		return NULL;

	if (sdxi_ring_try_reserve(cxt->ring_state, 1, &sddesc->resv))
		return NULL;

	copy = sdxi_ring_resv_next(&sddesc->resv);
	(void)sdxi_encode_copy(copy, params); /* Caller checked validity. */
	sdxi_completion_attach(copy, comp);

	sddesc->completion = no_free_ptr(comp);
	return_ptr(sddesc);
}

static struct dma_async_tx_descriptor *
sdxi_dma_prep_memcpy(struct dma_chan *dma_chan, dma_addr_t dst,
		     dma_addr_t src, size_t len, unsigned long flags)
{
	struct sdxi_akey_ent *akey = to_sdxi_dma_chan(dma_chan)->akey;
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	u16 akey_index = sdxi_akey_index(cxt, akey);
	struct sdxi_dma_desc *sddesc;
	struct sdxi_copy copy = {
		.src = src,
		.dst = dst,
		.src_akey = akey_index,
		.dst_akey = akey_index,
		.len = len,
	};

	/*
	 * Perform a trial encode to a dummy descriptor on the stack
	 * so we can reject bad inputs without touching the ring
	 * state.
	 */
	if (sdxi_encode_copy(&(struct sdxi_desc){}, &copy))
		return NULL;

	sddesc = (flags & DMA_PREP_INTERRUPT) ?
		prep_memcpy_intr(dma_chan, &copy) :
		prep_memcpy_polled(dma_chan, &copy);

	if (!sddesc)
		return NULL;

	return vchan_tx_prep(to_virt_chan(dma_chan), &sddesc->vdesc, flags);
}

static enum dma_status sdxi_tx_status(struct dma_chan *chan,
				      dma_cookie_t cookie,
				      struct dma_tx_state *state)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(chan);
	struct sdxi_dma_desc *sddesc;
	enum dma_status status;
	struct virt_dma_desc *vdesc;

	status = dma_cookie_status(chan, cookie, state);
	if (status == DMA_COMPLETE)
		return status;

	guard(spinlock_irqsave)(&sdchan->vchan.lock);

	vdesc = vchan_find_desc(&sdchan->vchan, cookie);
	if (!vdesc)
		return status;

	sddesc = to_sdxi_dma_desc(vdesc);

	if (WARN_ON_ONCE(!sddesc->completion))
		return DMA_ERROR;

	if (!sdxi_completion_signaled(sddesc->completion))
		return DMA_IN_PROGRESS;

	if (sdxi_completion_errored(sddesc->completion))
		return DMA_ERROR;

	list_del(&vdesc->node);
	vchan_cookie_complete(vdesc);

	return dma_cookie_status(chan, cookie, state);
}

static void sdxi_dma_issue_pending(struct dma_chan *dma_chan)
{
	struct virt_dma_chan *vchan = to_virt_chan(dma_chan);
	struct virt_dma_desc *vdesc;
	u64 dbval = 0;

	scoped_guard(spinlock_irqsave, &vchan->lock) {
		/*
		 * This can happen with racing submitters.
		 */
		if (list_empty(&vchan->desc_submitted))
			return;

		list_for_each_entry(vdesc, &vchan->desc_submitted, node) {
			struct sdxi_dma_desc *sddesc = to_sdxi_dma_desc(vdesc);
			struct sdxi_desc *hwdesc;

			sdxi_ring_resv_foreach(&sddesc->resv, hwdesc)
				sdxi_desc_make_valid(hwdesc);
			/*
			 * The reservations ought to be ordered
			 * ascending, but use umax() just in case.
			 */
			dbval = umax(sdxi_ring_resv_dbval(&sddesc->resv), dbval);
		}

		vchan_issue_pending(vchan);
	}

	/*
	 * The implementation is required to handle out-of-order
	 * doorbell updates; we can do this after dropping the
	 * lock.
	 */
	sdxi_cxt_push_doorbell(to_sdxi_dma_chan(dma_chan)->cxt, dbval);
}

static int sdxi_dma_terminate_all(struct dma_chan *dma_chan)
{
	struct virt_dma_chan *vchan = to_virt_chan(dma_chan);
	u64 dbval = 0;

	/*
	 * Allocated and submitted txds are in the ring but not valid
	 * yet. Overwrite them with nops and then set their valid
	 * bits.
	 *
	 * The implementation may start consuming these as soon as the
	 * valid bits flip. sdxi_dma_synchronize() will ensure they're
	 * all done.
	 */
	scoped_guard(spinlock_irqsave, &vchan->lock) {
		struct virt_dma_desc *vdesc;
		LIST_HEAD(head);

		list_splice_tail_init(&vchan->desc_allocated, &head);
		list_splice_tail_init(&vchan->desc_submitted, &head);

		if (list_empty(&head))
			return 0;

		list_for_each_entry(vdesc, &head, node) {
			struct sdxi_dma_desc *sddesc = to_sdxi_dma_desc(vdesc);
			struct sdxi_desc *hwdesc;

			sdxi_ring_resv_foreach(&sddesc->resv, hwdesc) {
				sdxi_serialize_nop(hwdesc);
				sdxi_desc_make_valid(hwdesc);
			}

			dbval = umax(sdxi_ring_resv_dbval(&sddesc->resv), dbval);
		}

		list_splice_tail(&head, &vchan->desc_terminated);
	}

	sdxi_cxt_push_doorbell(to_sdxi_dma_chan(dma_chan)->cxt, dbval);

	return 0;
}

static void sdxi_dma_synchronize(struct dma_chan *dma_chan)
{
	struct sdxi_cxt *cxt = to_sdxi_dma_chan(dma_chan)->cxt;
	struct sdxi_ring_resv resv;
	struct sdxi_desc *nop;
	int err;

	/* Submit a single nop with fence and wait for it to complete. */

	if (sdxi_ring_reserve(cxt->ring_state, 1, &resv))
		return;

	struct sdxi_completion *comp __free(sdxi_completion) = sdxi_completion_alloc(cxt->sdxi);
	if (!comp)
		return;

	nop = sdxi_ring_resv_next(&resv);
	sdxi_serialize_nop(nop);
	sdxi_completion_attach(nop, comp);
	sdxi_desc_set_fence(nop);
	sdxi_desc_make_valid(nop);
	sdxi_cxt_push_doorbell(cxt, sdxi_ring_resv_dbval(&resv));

	err = sdxi_completion_poll(comp);
	WARN_ONCE(err, "got %d polling cst_blk", err);

	vchan_synchronize(to_virt_chan(dma_chan));
}

static irqreturn_t sdxi_dma_cxt_irq(int irq, void *data)
{
	struct sdxi_dma_chan *sdchan = data;
	struct virt_dma_chan *vchan = &sdchan->vchan;
	struct virt_dma_desc *vdesc;
	bool completed = false;

	guard(spinlock_irqsave)(&vchan->lock);

	while ((vdesc = vchan_next_desc(vchan))) {
		struct sdxi_dma_desc *sddesc = to_sdxi_dma_desc(vdesc);

		if (!sdxi_completion_signaled(sddesc->completion))
			break;

		list_del(&vdesc->node);
		vchan_cookie_complete(&sddesc->vdesc);
		completed = true;
	}

	if (completed)
		sdxi_ring_wake_up(sdchan->cxt->ring_state);

	return IRQ_HANDLED;
}

static int sdxi_dma_alloc_chan_resources(struct dma_chan *dma_chan)
{
	struct sdxi_dev *sdxi = dev_get_drvdata(dma_chan->device->dev);
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);
	int vector, irq, err;

	sdchan->cxt = sdxi_cxt_new(sdxi);
	if (!sdchan->cxt)
		return -ENOMEM;
	/*
	 * This irq and akey setup should perhaps all be pushed into
	 * the context allocation.
	 */
	err = vector = sdxi_alloc_vector(sdxi);
	if (vector < 0)
		goto exit_cxt;

	sdchan->vector = vector;

	err = irq = sdxi_vector_to_irq(sdxi, vector);
	if (irq < 0)
		goto free_vector;

	sdchan->irq = irq;

	/*
	 * Note this akey entry is used for both the completion
	 * interrupt and source and destination access for copies.
	 */
	sdchan->akey = sdxi_alloc_akey(sdchan->cxt);
	if (!sdchan->akey)
		goto free_vector;

	*sdchan->akey = (typeof(*sdchan->akey)) {
		.intr_num = cpu_to_le16(FIELD_PREP(SDXI_AKEY_ENT_VL, 1) |
					FIELD_PREP(SDXI_AKEY_ENT_IV, 1) |
					FIELD_PREP(SDXI_AKEY_ENT_INTR_NUM,
						   vector)),
	};

	err = request_irq(sdchan->irq, sdxi_dma_cxt_irq,
			  IRQF_TRIGGER_NONE, "SDXI DMAengine", sdchan);
	if (err)
		goto free_akey;

	err = sdxi_start_cxt(sdchan->cxt);
	if (err)
		goto free_irq;

	return 0;
free_irq:
	free_irq(sdchan->irq, sdchan);
free_akey:
	sdxi_free_akey(sdchan->cxt, sdchan->akey);
free_vector:
	sdxi_free_vector(sdxi, vector);
exit_cxt:
	sdxi_cxt_exit(sdchan->cxt);
	return err;
}

static void sdxi_dma_free_chan_resources(struct dma_chan *dma_chan)
{
	struct sdxi_dma_chan *sdchan = to_sdxi_dma_chan(dma_chan);

	sdxi_stop_cxt(sdchan->cxt);
	free_irq(sdchan->irq, sdchan);
	sdxi_free_vector(sdchan->cxt->sdxi, sdchan->vector);
	sdxi_free_akey(sdchan->cxt, sdchan->akey);
	vchan_free_chan_resources(to_virt_chan(dma_chan));
	sdxi_cxt_exit(sdchan->cxt);
}

int sdxi_dma_register(struct sdxi_dev *sdxi)
{
	struct device *dev = sdxi->dev;
	struct sdxi_dma_dev *sddev;
	struct dma_device *dma_dev;
	int err;

	if (!dma_channels)
		return 0;
	/*
	 * Note that this code assumes the device supports the
	 * interrupt operation group (IntrGrp), which is optional. See
	 * SDXI 1.0 Table 6-1 SDXI Operation Groups.
	 *
	 * TODO: check sdxi->op_grp_cap for IntrGrp support and error
	 * out if it's missing.
	 */

	sddev = devm_kzalloc(dev, struct_size(sddev, sdchan, dma_channels),
			     GFP_KERNEL);
	if (!sddev)
		return -ENOMEM;

	sddev->nr_channels = dma_channels;

	dma_dev = &sddev->dma_dev;
	*dma_dev = (typeof(*dma_dev)) {
		.dev                 = dev,
		.src_addr_widths     = DMA_SLAVE_BUSWIDTH_64_BYTES,
		.dst_addr_widths     = DMA_SLAVE_BUSWIDTH_64_BYTES,
		.directions          = BIT(DMA_MEM_TO_MEM),
		.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR,

		.device_alloc_chan_resources = sdxi_dma_alloc_chan_resources,
		.device_free_chan_resources  = sdxi_dma_free_chan_resources,

		.device_prep_dma_memcpy = sdxi_dma_prep_memcpy,

		.device_terminate_all = sdxi_dma_terminate_all,
		.device_synchronize = sdxi_dma_synchronize,
		.device_tx_status = sdxi_tx_status,
		.device_issue_pending = sdxi_dma_issue_pending,
	};

	dma_cap_set(DMA_MEMCPY, dma_dev->cap_mask);
	INIT_LIST_HEAD(&dma_dev->channels);

	for (size_t i = 0; i < sddev->nr_channels; ++i) {
		struct sdxi_dma_chan *sdchan = &sddev->sdchan[i];

		sdchan->vchan.desc_free = sdxi_tx_desc_free;
		vchan_init(&sdchan->vchan, &sddev->dma_dev);
	}

	err = dmaenginem_async_device_register(dma_dev);
	if (err)
		return dev_warn_probe(dev, err, "failed to register dma device\n");

	return 0;
}
