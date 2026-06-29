// SPDX-License-Identifier: GPL-2.0-only
#include <linux/compiler.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/minmax.h>
#include <linux/string.h>

#include "hws_irq.h"
#include "hws_reg.h"
#include "hws_video.h"
#include "hws.h"

#define MAX_INT_LOOPS 100

static bool hws_toggle_debug;
module_param_named(toggle_debug, hws_toggle_debug, bool, 0644);
MODULE_PARM_DESC(toggle_debug,
		 "Read toggle registers in IRQ handler for debug logging");

static int hws_arm_next(struct hws_pcie_dev *hws, u32 ch)
{
	struct hws_video *v = &hws->video[ch];
	unsigned long flags;
	struct hwsvideo_buffer *buf;

	dev_dbg(&hws->pdev->dev,
		"arm_next(ch=%u): stop=%d cap=%d queued=%d\n",
		ch, READ_ONCE(v->stop_requested), READ_ONCE(v->cap_active),
		!list_empty(&v->capture_queue));

	if (READ_ONCE(hws->suspended)) {
		dev_dbg(&hws->pdev->dev, "arm_next(ch=%u): suspended\n", ch);
		return -EBUSY;
	}

	if (READ_ONCE(v->stop_requested) || !READ_ONCE(v->cap_active)) {
		dev_dbg(&hws->pdev->dev,
			"arm_next(ch=%u): stop=%d cap=%d -> cancel\n", ch,
			v->stop_requested, v->cap_active);
		return -ECANCELED;
	}

	spin_lock_irqsave(&v->irq_lock, flags);
	if (v->active) {
		buf = v->active;
		spin_unlock_irqrestore(&v->irq_lock, flags);
		dev_dbg(&hws->pdev->dev,
			"arm_next(ch=%u): active buffer already armed %p\n",
			ch, buf);
		return 0;
	}
	if (v->next_prepared) {
		buf = v->next_prepared;
		v->active = buf;
		v->next_prepared = NULL;
		spin_unlock_irqrestore(&v->irq_lock, flags);
		dev_dbg(&hws->pdev->dev,
			"arm_next(ch=%u): promoted prepared buffer %p\n",
			ch, buf);
		return 0;
	}
	if (list_empty(&v->capture_queue)) {
		spin_unlock_irqrestore(&v->irq_lock, flags);
		dev_dbg(&hws->pdev->dev, "arm_next(ch=%u): queue empty\n", ch);
		return -EAGAIN;
	}

	buf = list_first_entry(&v->capture_queue, struct hwsvideo_buffer, list);
	list_del_init(&buf->list);	/* keep buffer safe for later cleanup */
	if (v->queued_count)
		v->queued_count--;
	v->active = buf;
	spin_unlock_irqrestore(&v->irq_lock, flags);
	dev_dbg(&hws->pdev->dev, "arm_next(ch=%u): picked buffer %p\n", ch,
		buf);

	/* Publish descriptor(s) before MMIO capture updates. */
	wmb();

	/* Avoid MMIO during suspend */
	if (READ_ONCE(hws->suspended)) {
		unsigned long f;

		dev_dbg(&hws->pdev->dev,
			"arm_next(ch=%u): suspended after pick\n", ch);
		spin_lock_irqsave(&v->irq_lock, f);
		if (v->active == buf) {
			list_add(&buf->list, &v->capture_queue);
			v->queued_count++;
			v->active = NULL;
		}
		spin_unlock_irqrestore(&v->irq_lock, f);
		return -EBUSY;
	}

	/* Program the baseline DMA window; use arena bounce if needed. */
	{
		int ret = hws_program_dma_for_buffer(hws, ch, buf);

		if (ret) {
			unsigned long f;

			spin_lock_irqsave(&v->irq_lock, f);
			if (v->active == buf) {
				v->active = NULL;
				list_add(&buf->list, &v->capture_queue);
				v->queued_count++;
			}
			spin_unlock_irqrestore(&v->irq_lock, f);
			return ret;
		}
	}

	dev_dbg(&hws->pdev->dev, "arm_next(ch=%u): programmed buffer %p\n", ch,
		buf);
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
	struct hwsvideo_buffer *promoted_active = NULL;
	unsigned long flags;
	bool promoted = false;
	int ret;

	dev_dbg(&hws->pdev->dev,
		"bh_video(ch=%u): stop=%d cap=%d active=%p\n",
		ch, READ_ONCE(v->stop_requested), READ_ONCE(v->cap_active),
		v->active);

	dev_dbg(&hws->pdev->dev,
		"bh_video(ch=%u): entry stop=%d cap=%d\n", ch,
		v->stop_requested, v->cap_active);
	if (READ_ONCE(hws->suspended))
		return;

	if (READ_ONCE(v->stop_requested) || !READ_ONCE(v->cap_active))
		return;

	spin_lock_irqsave(&v->irq_lock, flags);
	done = v->active;
	if (done && v->next_prepared) {
		v->active = v->next_prepared;
		v->next_prepared = NULL;
		promoted_active = v->active;
		promoted = true;
	}
	spin_unlock_irqrestore(&v->irq_lock, flags);

	/* 1) Complete the buffer the HW just finished (if any) */
	if (done) {
		struct vb2_v4l2_buffer *vb2v = &done->vb;
		enum vb2_buffer_state state = VB2_BUF_STATE_DONE;

		ret = hws_video_prepare_done_buffer(v, done);
		if (ret) {
			dev_warn_ratelimited(&hws->pdev->dev,
					     "bh_video(ch=%u): failed to prepare completed buffer ret=%d\n",
					     ch, ret);
			state = VB2_BUF_STATE_ERROR;
		} else {
			dev_dbg(&hws->pdev->dev,
				"bh_video(ch=%u): DONE buf=%p seq=%u half_seen=%d toggle=%u\n",
				ch, done, vb2v->sequence, v->half_seen,
				v->last_buf_half_toggle);
		}

		spin_lock_irqsave(&v->irq_lock, flags);
		if (v->active == done) {
			if (v->next_prepared) {
				v->active = v->next_prepared;
				v->next_prepared = NULL;
				promoted_active = v->active;
				promoted = true;
			} else {
				v->active = NULL;
			}
		} else if (v->active) {
			promoted_active = v->active;
			promoted = true;
		}
		spin_unlock_irqrestore(&v->irq_lock, flags);

		vb2_buffer_done(&vb2v->vb2_buf, state);
	}

	if (READ_ONCE(hws->suspended))
		return;

	if (promoted) {
		dev_dbg(&hws->pdev->dev,
			"bh_video(ch=%u): promoted pre-armed buffer active=%p\n",
			ch, promoted_active);
		spin_lock_irqsave(&v->irq_lock, flags);
		ret = hws_prime_next_locked(v);
		spin_unlock_irqrestore(&v->irq_lock, flags);
		if (ret)
			dev_warn_ratelimited(&hws->pdev->dev,
					     "bh_video(ch=%u): failed to pre-arm next buffer ret=%d\n",
					     ch, ret);
		return;
	}

	/* 2) Immediately arm the next queued buffer (if present) */
	ret = hws_arm_next(hws, ch);
	if (ret == -EAGAIN) {
		dev_dbg(&hws->pdev->dev,
			"bh_video(ch=%u): no queued buffer to arm\n", ch);
		return;
	}
	if (ret) {
		dev_warn_ratelimited(&hws->pdev->dev,
				     "bh_video(ch=%u): stopping video queue after DMA arm failure ret=%d\n",
				     ch, ret);
		hws_enable_video_capture(hws, ch, false);
		WRITE_ONCE(v->cap_active, false);
		WRITE_ONCE(v->stop_requested, true);
		vb2_queue_error(&v->buffer_queue);
		return;
	}
	dev_dbg(&hws->pdev->dev,
		"bh_video(ch=%u): armed next buffer, active=%p\n", ch,
		v->active);
	/* On success the engine now points at v->active's DMA address */
}

static void hws_irq_ack_status(struct hws_pcie_dev *pdx, u32 int_state)
{
	if (!int_state || !pdx || !pdx->bar0_base)
		return;

	writel(int_state, pdx->bar0_base + HWS_REG_INT_STATUS);
	(void)readl(pdx->bar0_base + HWS_REG_INT_STATUS);
}

static void hws_irq_record_vdone(struct hws_pcie_dev *pdx, unsigned int ch)
{
	unsigned long flags;

	if (!pdx || ch >= MAX_VID_CHANNELS)
		return;

	spin_lock_irqsave(&pdx->irq_thread_lock, flags);
	pdx->irq_pending_vdone[ch]++;
	spin_unlock_irqrestore(&pdx->irq_thread_lock, flags);
}

static bool hws_irq_take_vdone(struct hws_pcie_dev *pdx, unsigned int *ch)
{
	unsigned long flags;
	unsigned int i;

	if (!pdx || !ch)
		return false;

	spin_lock_irqsave(&pdx->irq_thread_lock, flags);
	for (i = 0; i < pdx->cur_max_video_ch && i < MAX_VID_CHANNELS; i++) {
		if (pdx->irq_pending_vdone[i]) {
			pdx->irq_pending_vdone[i]--;
			*ch = i;
			spin_unlock_irqrestore(&pdx->irq_thread_lock, flags);
			return true;
		}
	}
	spin_unlock_irqrestore(&pdx->irq_thread_lock, flags);
	return false;
}

static bool hws_irq_queue_video(struct hws_pcie_dev *pdx, u32 int_state)
{
	bool wake_thread = false;
	unsigned int ch;

	for (ch = 0; ch < pdx->cur_max_video_ch; ++ch) {
		u32 vbit = HWS_INT_VDONE_BIT(ch);

		if (!(int_state & vbit))
			continue;

		if (READ_ONCE(pdx->video[ch].cap_active) &&
		    !READ_ONCE(pdx->video[ch].stop_requested)) {
			if (hws_toggle_debug) {
				u32 toggle =
				    readl_relaxed(pdx->bar0_base +
						  HWS_REG_VBUF_TOGGLE(ch)) & 0x01;

				WRITE_ONCE(pdx->video[ch].last_buf_half_toggle,
					   toggle);
			}
			WRITE_ONCE(pdx->video[ch].half_seen, true);
			hws_irq_record_vdone(pdx, ch);
			wake_thread = true;
			dev_dbg(&pdx->pdev->dev,
				"irq: VDONE ch=%u queued for threaded completion\n",
				ch);
		} else {
			dev_dbg(&pdx->pdev->dev,
				"irq: VDONE ch=%u ignored (cap=%d stop=%d)\n",
				ch,
				READ_ONCE(pdx->video[ch].cap_active),
				READ_ONCE(pdx->video[ch].stop_requested));
		}
	}

	return wake_thread;
}

irqreturn_t hws_irq_handler(int irq, void *info)
{
	struct hws_pcie_dev *pdx = info;
	u32 int_state;
	bool wake_thread;

	(void)irq;

	if (!pdx || !pdx->bar0_base)
		return IRQ_NONE;

	dev_dbg(&pdx->pdev->dev, "irq: entry\n");
	if (pdx->bar0_base) {
		dev_dbg(&pdx->pdev->dev,
			"irq: INT_EN=0x%08x INT_STATUS=0x%08x\n",
			readl(pdx->bar0_base + INT_EN_REG_BASE),
			readl(pdx->bar0_base + HWS_REG_INT_STATUS));
	}

	/* Fast path: if suspended, quietly ack and exit */
	if (READ_ONCE(pdx->suspended)) {
		int_state = readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
		if (int_state)
			hws_irq_ack_status(pdx, int_state);
		return int_state ? IRQ_HANDLED : IRQ_NONE;
	}

	int_state = readl_relaxed(pdx->bar0_base + HWS_REG_INT_STATUS);
	if (!int_state || int_state == 0xFFFFFFFF) {
		dev_dbg(&pdx->pdev->dev,
			"irq: spurious or device-gone int_state=0x%08x\n",
			int_state);
		return IRQ_NONE;
	}
	dev_dbg(&pdx->pdev->dev, "irq: entry INT_STATUS=0x%08x\n", int_state);

	wake_thread = hws_irq_queue_video(pdx, int_state);
	hws_irq_ack_status(pdx, int_state);

	return wake_thread ? IRQ_WAKE_THREAD : IRQ_HANDLED;
}

irqreturn_t hws_irq_thread(int irq, void *info)
{
	struct hws_pcie_dev *pdx = info;
	unsigned int ch;
	unsigned int count = 0;
	bool handled = false;

	(void)irq;

	if (!pdx || !pdx->bar0_base)
		return IRQ_NONE;

	while (hws_irq_take_vdone(pdx, &ch)) {
		handled = true;
		if (READ_ONCE(pdx->suspended))
			continue;

		hws_video_handle_vdone(&pdx->video[ch]);
		count++;
		if (count == MAX_INT_LOOPS)
			dev_warn_ratelimited(&pdx->pdev->dev,
					     "threaded IRQ processing many VDONE events\n");
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}
