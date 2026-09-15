// SPDX-License-Identifier: GPL-2.0-only
#include <linux/compiler.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include <media/videobuf2-dma-contig.h>

#include "hws_irq.h"
#include "hws_reg.h"
#include "hws_video.h"
#include "hws.h"

#define MAX_INT_LOOPS 100

static int hws_arm_next(struct hws_pcie_dev *hws, u32 ch)
{
	struct hws_video *v = &hws->video[ch];
	unsigned long flags;
	struct hwsvideo_buffer *buf;

	if (READ_ONCE(hws->suspended)) {
		return -EBUSY;
	}

	if (READ_ONCE(v->stop_requested) || !READ_ONCE(v->cap_active)) {
		return -ECANCELED;
	}

	spin_lock_irqsave(&v->irq_lock, flags);
	if (list_empty(&v->capture_queue)) {
		spin_unlock_irqrestore(&v->irq_lock, flags);
		return -EAGAIN;
	}

	buf = list_first_entry(&v->capture_queue, struct hwsvideo_buffer, list);
	list_del_init(&buf->list);	/* keep buffer safe for later cleanup */
	if (v->queued_count)
		v->queued_count--;
	v->active = buf;
	spin_unlock_irqrestore(&v->irq_lock, flags);

	/* Publish descriptor(s) before doorbell/MMIO kicks. */
	wmb();

	/* Avoid MMIO during suspend */
	if (READ_ONCE(hws->suspended)) {
		unsigned long f;

		spin_lock_irqsave(&v->irq_lock, f);
		if (v->active) {
			list_add(&buf->list, &v->capture_queue);
			v->queued_count++;
			v->active = NULL;
		}
		spin_unlock_irqrestore(&v->irq_lock, f);
		return -EBUSY;
	}

	/* Also program the DMA address register directly */
	{
		dma_addr_t dma_addr =
		    vb2_dma_contig_plane_dma_addr(&buf->vb.vb2_buf, 0);
		hws_program_dma_for_addr(hws, ch, dma_addr);
		iowrite32(lower_32_bits(dma_addr),
			  hws->bar0_base + HWS_REG_DMA_ADDR(ch));
	}

	spin_lock_irqsave(&v->irq_lock, flags);
	hws_prime_next_locked(v);
	spin_unlock_irqrestore(&v->irq_lock, flags);
	return 0;
}

static void hws_video_handle_vdone(struct hws_video *v)
{
	struct hws_pcie_dev *hws = v->parent;
	unsigned int ch = v->channel_index;
	struct hwsvideo_buffer *done;
	unsigned long flags;
	bool promoted = false;

	int ret;

	if (READ_ONCE(hws->suspended))
		return;

	if (READ_ONCE(v->stop_requested) || !READ_ONCE(v->cap_active))
		return;

	spin_lock_irqsave(&v->irq_lock, flags);
	done = v->active;
	if (done && v->next_prepared) {
		v->active = v->next_prepared;
		v->next_prepared = NULL;
		promoted = true;
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);

	/* 1) Complete the buffer the HW just finished (if any) */
	if (done) {
		struct vb2_v4l2_buffer *vb2v = &done->vb;
		size_t expected = v->pix.sizeimage;
		size_t plane_size = vb2_plane_size(&vb2v->vb2_buf, 0);

		if (expected > plane_size) {
			dev_warn_ratelimited(&hws->pdev->dev,
					     "bh_video(ch=%u): sizeimage %zu > plane %zu, dropping seq=%u\n",
					     ch, expected, plane_size,
					     (u32)atomic_read(&v->sequence_number) + 1);
			vb2_buffer_done(&vb2v->vb2_buf, VB2_BUF_STATE_ERROR);
			goto arm_next;
		}
		vb2_set_plane_payload(&vb2v->vb2_buf, 0, expected);

		dma_rmb();	/* device writes visible before userspace sees it */

		vb2v->sequence = (u32)atomic_inc_return(&v->sequence_number);
		vb2v->vb2_buf.timestamp = ktime_get_ns();

		if (!promoted)
			v->active = NULL;	/* channel no longer owns this buffer */
		vb2_buffer_done(&vb2v->vb2_buf, VB2_BUF_STATE_DONE);
	}

	if (READ_ONCE(hws->suspended))
		return;

	if (promoted) {
		spin_lock_irqsave(&v->irq_lock, flags);
		hws_prime_next_locked(v);
		spin_unlock_irqrestore(&v->irq_lock, flags);
		return;
	}

arm_next:
	/* 2) Immediately arm the next queued buffer (if present) */
	ret = hws_arm_next(hws, ch);
	if (ret == -EAGAIN) {
		return;
	}
	/* On success the engine now points at v->active's DMA address */
}

irqreturn_t hws_irq_handler(int irq, void *info)
{
	struct hws_pcie_dev *pdx = info;
	u32 int_state;

	/* Fast path: if suspended, quietly ack and exit */
	if (READ_ONCE(pdx->suspended)) {
		int_state = readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
		if (int_state) {
			writel(int_state, pdx->bar0_base + HWS_REG_INT_STATUS);
			(void)readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
		}
		return int_state ? IRQ_HANDLED : IRQ_NONE;
	}
	int_state = readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
	if (!int_state || int_state == 0xFFFFFFFF) {
		return IRQ_NONE;
	}

	/* Loop until all pending bits are serviced (max 100 iterations) */
	for (u32 cnt = 0; int_state && cnt < MAX_INT_LOOPS; ++cnt) {
		for (unsigned int ch = 0; ch < pdx->cur_max_video_ch; ++ch) {
			u32 vbit = HWS_INT_VDONE_BIT(ch);

			if (!(int_state & vbit))
				continue;

			if (READ_ONCE(pdx->video[ch].cap_active) &&
			    !READ_ONCE(pdx->video[ch].stop_requested)) {
				dma_rmb();
				WRITE_ONCE(pdx->video[ch].half_seen, true);
				hws_video_handle_vdone(&pdx->video[ch]);
			}

			writel(vbit, pdx->bar0_base + HWS_REG_INT_STATUS);
			(void)readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
		}

		/* Re-read in case new interrupt bits popped while processing */
		int_state = readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
		if (cnt + 1 == MAX_INT_LOOPS)
			dev_warn_ratelimited(&pdx->pdev->dev,
					     "IRQ storm? status=0x%08x\n",
					     int_state);
	}

	return IRQ_HANDLED;
}
