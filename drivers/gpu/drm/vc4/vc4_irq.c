/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * DOC: Interrupt management for the V3D engine
 *
 * We have an interrupt status register (V3D_INTCTL) which reports
 * interrupts, and where writing 1 bits clears those interrupts.
 * There are also a pair of interrupt registers
 * (V3D_INTENA/V3D_INTDIS) where writing a 1 to their bits enables or
 * disables that specific interrupt, and 0s written are ignored
 * (reading either one returns the set of enabled interrupts).
 *
 * When we take a binning flush done interrupt, we need to submit the
 * next frame for binning and move the finished frame to the render
 * thread.
 *
 * When we take a render frame interrupt, we need to wake the
 * processes waiting for some frame to be done, and get the next frame
 * submitted ASAP (so the hardware doesn't sit idle when there's work
 * to do).
 *
 * When we take the binner out of memory interrupt, we need to
 * allocate some new memory and pass it to the binner so that the
 * current job can make progress.
 */

#include <linux/platform_device.h>

#include <drm/drm_print.h>

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "vc4_trace.h"

#define V3D_DRIVER_IRQS (V3D_INT_OUTOMEM | \
			 V3D_INT_FLDONE | \
			 V3D_INT_FRDONE)

static void
vc4_overflow_mem_work(struct work_struct *work)
{
	struct vc4_dev *vc4 =
		container_of(work, struct vc4_dev, overflow_mem_work);
	struct vc4_bo *bo;
	int bin_bo_slot;
	unsigned long irqflags;

	mutex_lock(&vc4->bin_bo_lock);

	if (!vc4->bin_bo)
		goto complete;

	bo = vc4->bin_bo;

	bin_bo_slot = vc4_v3d_get_bin_slot(vc4);
	if (bin_bo_slot < 0) {
		drm_err(&vc4->base, "Couldn't allocate binner overflow mem\n");
		goto complete;
	}

	spin_lock_irqsave(&vc4->job_lock, irqflags);

	if (vc4->bin_alloc_overflow) {
		/* If we had overflow memory allocated previously,
		 * then that chunk will free when the current render job
		 * is done. If we don't have a render job running, then
		 * the chunk is free immediately.
		 */
		if (vc4->bin_job) {
			vc4->bin_job->render->bin_slots |= vc4->bin_alloc_overflow;
		} else if (vc4->render_job) {
			vc4->render_job->bin_slots |= vc4->bin_alloc_overflow;
		} else {
			/* There's nothing queued in the hardware, so
			 * the old slot is free immediately.
			 */
			vc4->bin_alloc_used &= ~vc4->bin_alloc_overflow;
		}
	}
	vc4->bin_alloc_overflow = BIT(bin_bo_slot);

	V3D_WRITE(V3D_BPOA, bo->base.dma_addr + bin_bo_slot * vc4->bin_alloc_size);
	V3D_WRITE(V3D_BPOS, bo->base.base.size);
	V3D_WRITE(V3D_INTCTL, V3D_INT_OUTOMEM);
	V3D_WRITE(V3D_INTENA, V3D_INT_OUTOMEM);
	spin_unlock_irqrestore(&vc4->job_lock, irqflags);

complete:
	mutex_unlock(&vc4->bin_bo_lock);
}

static irqreturn_t
vc4_irq(int irq, void *arg)
{
	struct drm_device *dev = arg;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	uint32_t intctl;
	irqreturn_t status = IRQ_NONE;

	barrier();
	intctl = V3D_READ(V3D_INTCTL);

	/* Acknowledge the interrupts we're handling here. The binner
	 * last flush / render frame done interrupt will be cleared,
	 * while OUTOMEM will stay high until the underlying cause is
	 * cleared.
	 */
	V3D_WRITE(V3D_INTCTL, intctl);

	if (intctl & V3D_INT_OUTOMEM) {
		/* Disable OUTOMEM until the work is done. */
		V3D_WRITE(V3D_INTDIS, V3D_INT_OUTOMEM);
		schedule_work(&vc4->overflow_mem_work);
		status = IRQ_HANDLED;
	}

	if (intctl & V3D_INT_FLDONE) {
		struct vc4_bin_job *bin_job;
		struct vc4_fence *fence;

		spin_lock(&vc4->job_lock);
		bin_job = vc4->bin_job;
		vc4->bin_job = NULL;
		spin_unlock(&vc4->job_lock);

		fence = to_vc4_fence(bin_job->base.irq_fence);

		vc4_perfmon_stop(vc4, bin_job->base.perfmon, true);

		trace_vc4_bcl_end_irq(dev, fence->seqno);

		dma_fence_signal(&fence->base);

		status = IRQ_HANDLED;
	}

	if (intctl & V3D_INT_FRDONE) {
		struct vc4_render_job *render_job;
		struct vc4_fence *fence;

		spin_lock(&vc4->job_lock);
		render_job = vc4->render_job;
		vc4->render_job = NULL;
		spin_unlock(&vc4->job_lock);

		fence = to_vc4_fence(render_job->base.irq_fence);

		vc4_perfmon_stop(vc4, render_job->base.perfmon, true);

		trace_vc4_rcl_end_irq(dev, fence->seqno);

		dma_fence_signal(&fence->base);

		status = IRQ_HANDLED;
	}

	return status;
}

void
vc4_irq_enable(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return;

	if (!vc4->v3d)
		return;

	/* Enable the render done interrupts. The out-of-memory interrupt is
	 * enabled as soon as we have a binner BO allocated.
	 */
	V3D_WRITE(V3D_INTENA, V3D_INT_FLDONE | V3D_INT_FRDONE);
}

void
vc4_irq_disable(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return;

	if (!vc4->v3d)
		return;

	/* Disable sending interrupts for our driver's IRQs. */
	V3D_WRITE(V3D_INTDIS, V3D_DRIVER_IRQS);

	/* Clear any pending interrupts we might have left. */
	V3D_WRITE(V3D_INTCTL, V3D_DRIVER_IRQS);

	/* Finish any interrupt handler still in flight. */
	synchronize_irq(vc4->irq);

	cancel_work_sync(&vc4->overflow_mem_work);
}

int vc4_irq_install(struct drm_device *dev, int irq)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	if (!vc4->v3d)
		return -ENODEV;

	if (irq == IRQ_NOTCONNECTED)
		return -ENOTCONN;

	INIT_WORK(&vc4->overflow_mem_work, vc4_overflow_mem_work);

	/* Clear any pending interrupts someone might have left around
	 * for us.
	 */
	V3D_WRITE(V3D_INTCTL, V3D_DRIVER_IRQS);

	ret = devm_request_irq(dev->dev, irq, vc4_irq, 0,
			       dev_name(dev->dev), dev);
	if (ret)
		return ret;

	vc4_irq_enable(dev);

	return 0;
}

void vc4_irq_uninstall(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return;

	vc4_irq_disable(dev);
}

/** Reinitializes interrupt registers when a GPU reset is performed. */
void vc4_irq_reset(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return;

	/* Acknowledge any stale IRQs. */
	V3D_WRITE(V3D_INTCTL, V3D_DRIVER_IRQS);

	/*
	 * Turn all our interrupts on.  Binner out of memory is the
	 * only one we expect to trigger at this point, since we've
	 * just come from poweron and haven't supplied any overflow
	 * memory yet.
	 */
	V3D_WRITE(V3D_INTENA, V3D_DRIVER_IRQS);
}
