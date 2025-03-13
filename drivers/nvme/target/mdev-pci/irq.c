// SPDX-License-Identifier: GPL-2.0+
/*
 * NVMe virtual controller IRQ implementation (MSIx and INTx)
 * Copyright (c) 2019 - Maxim Levitsky
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "priv.h"

/* Setup the interrupt subsystem */
void nvmet_mdev_irqs_setup(struct nvmet_mdev_vctrl *vctrl)
{
	vctrl->irqs.mode = NVME_MDEV_IMODE_NONE;
	vctrl->irqs.irq_coalesc_max = 1;
}

/* Enable INTx or MSIx interrupts  */
static int __nvmet_mdev_irqs_enable(struct nvmet_mdev_vctrl *vctrl,
				    enum nvmet_mdev_irq_mode mode)
{
	bool paused;

	if (vctrl->irqs.mode == mode)
		return 0;
	if (vctrl->irqs.mode != NVME_MDEV_IMODE_NONE)
		return -EBUSY;

	if (mode == NVME_MDEV_IMODE_INTX)
		_DBG(vctrl, "IRQ: enable INTx interrupts\n");
	else if (mode == NVME_MDEV_IMODE_MSIX)
		_DBG(vctrl, "IRQ: enable MSIX interrupts\n");
	else
		WARN_ON(1);

	paused = nvmet_mdev_io_pause(vctrl);
	vctrl->irqs.mode = mode;
	if (paused)
		nvmet_mdev_io_resume(vctrl);
	return 0;
}

int nvmet_mdev_irqs_enable(struct nvmet_mdev_vctrl *vctrl,
			   enum nvmet_mdev_irq_mode mode)
{
	int retval;

	mutex_lock(&vctrl->lock);
	retval = __nvmet_mdev_irqs_enable(vctrl, mode);
	mutex_unlock(&vctrl->lock);
	return retval;
}

/* Disable INTx or MSIx interrupts  */
static void __nvmet_mdev_irqs_disable(struct nvmet_mdev_vctrl *vctrl,
				      enum nvmet_mdev_irq_mode mode)
{
	unsigned int i;
	bool paused;

	if (vctrl->irqs.mode == NVME_MDEV_IMODE_NONE)
		return;
	if (vctrl->irqs.mode != mode)
		return;

	if (vctrl->irqs.mode == NVME_MDEV_IMODE_INTX)
		_DBG(vctrl, "IRQ: disable INTx interrupts\n");
	else if (vctrl->irqs.mode == NVME_MDEV_IMODE_MSIX)
		_DBG(vctrl, "IRQ: disable MSIX interrupts\n");
	else
		WARN_ON(1);

	paused = nvmet_mdev_io_pause(vctrl);

	for (i = 0; i < MAX_VIRTUAL_IRQS; i++) {
		struct nvmet_mdev_user_irq *vec = &vctrl->irqs.vecs[i];

		if (vec->trigger) {
			eventfd_ctx_put(vec->trigger);
			vec->trigger = NULL;
		}
		vec->irq_pending_cnt = 0;
		vec->irq_time = 0;
	}
	vctrl->irqs.mode = NVME_MDEV_IMODE_NONE;
	if (paused)
		nvmet_mdev_io_resume(vctrl);
}

void nvmet_mdev_irqs_disable(struct nvmet_mdev_vctrl *vctrl,
			     enum nvmet_mdev_irq_mode mode)
{
	mutex_lock(&vctrl->lock);
	__nvmet_mdev_irqs_disable(vctrl, mode);
	mutex_unlock(&vctrl->lock);
}

/* Set eventfd triggers for INTx or MSIx interrupts */
int nvmet_mdev_irqs_set_triggers(struct nvmet_mdev_vctrl *vctrl, int start,
				 int count, int32_t *fds)
{
	unsigned int i;
	bool paused;

	mutex_lock(&vctrl->lock);
	paused = nvmet_mdev_io_pause(vctrl);

	for (i = 0; i < count; i++) {
		int irqindex = start + i;
		struct eventfd_ctx *trigger;
		struct nvmet_mdev_user_irq *irq = &vctrl->irqs.vecs[irqindex];

		if (irq->trigger) {
			eventfd_ctx_put(irq->trigger);
			irq->trigger = NULL;
		}

		if (fds[i] < 0)
			continue;

		trigger = eventfd_ctx_fdget(fds[i]);
		if (IS_ERR(trigger)) {
			mutex_unlock(&vctrl->lock);
			return PTR_ERR(trigger);
		}

		irq->trigger = trigger;
	}
	if (paused)
		nvmet_mdev_io_resume(vctrl);
	mutex_unlock(&vctrl->lock);
	return 0;
}

/* Set eventfd trigger for unplug interrupt */
static int __nvmet_mdev_irqs_set_unplug_trigger(struct nvmet_mdev_vctrl *vctrl,
						int32_t fd)
{
	struct eventfd_ctx *trigger;

	if (vctrl->irqs.request_trigger) {
		_DBG(vctrl, "IRQ: clear hotplug trigger\n");
		eventfd_ctx_put(vctrl->irqs.request_trigger);
		vctrl->irqs.request_trigger = NULL;
	}

	if (fd < 0)
		return 0;

	_DBG(vctrl, "IRQ: set hotplug trigger\n");

	trigger = eventfd_ctx_fdget(fd);
	if (IS_ERR(trigger))
		return PTR_ERR(trigger);

	vctrl->irqs.request_trigger = trigger;
	return 0;
}

int nvmet_mdev_irqs_set_unplug_trigger(struct nvmet_mdev_vctrl *vctrl,
				       int32_t fd)
{
	int retval;

	mutex_lock(&vctrl->lock);
	retval = __nvmet_mdev_irqs_set_unplug_trigger(vctrl, fd);
	mutex_unlock(&vctrl->lock);
	return retval;
}

/* Reset the interrupts subsystem */
void nvmet_mdev_irqs_reset(struct nvmet_mdev_vctrl *vctrl)
{
	int i;

	lockdep_assert_held(&vctrl->lock);

	if (vctrl->irqs.mode != NVME_MDEV_IMODE_NONE)
		__nvmet_mdev_irqs_disable(vctrl, vctrl->irqs.mode);

	__nvmet_mdev_irqs_set_unplug_trigger(vctrl, -1);

	for (i = 0; i < MAX_VIRTUAL_IRQS; i++) {
		struct nvmet_mdev_user_irq *vec = &vctrl->irqs.vecs[i];

		vec->irq_coalesc_en = false;
		vec->irq_pending_cnt = 0;
		vec->irq_time = 0;
	}

	vctrl->irqs.irq_coalesc_time_us = 0;
}

/* Check if interrupt can be coalesced */
static bool nvmet_mdev_irq_coalesce(struct nvmet_mdev_vctrl *vctrl,
				    struct nvmet_mdev_user_irq *irq,
				    ktime_t now)
{
	s64 delta;

	if (!irq->irq_coalesc_en)
		return false;

	if (irq->irq_pending_cnt >= vctrl->irqs.irq_coalesc_max)
		return false;

	delta = ktime_us_delta(now, irq->irq_time);
	return (delta < vctrl->irqs.irq_coalesc_time_us);
}

void nvmet_mdev_irq_raise_unplug_event(struct nvmet_mdev_vctrl *vctrl)
{
	mutex_lock(&vctrl->lock);

	if (vctrl->irqs.request_trigger) {
		dev_notice_ratelimited(mdev_dev(vctrl->mdev),
				       "Relaying device request to user\n");
		eventfd_signal(vctrl->irqs.request_trigger);

	} else {
		dev_notice(mdev_dev(vctrl->mdev),
			   "No device request channel registered, blocked until released by user\n");
	}
	mutex_unlock(&vctrl->lock);
}

/* Raise an interrupt */
void nvmet_mdev_irq_raise(struct nvmet_mdev_vctrl *vctrl, unsigned int index)
{
	struct nvmet_mdev_user_irq *irq = &vctrl->irqs.vecs[index];

	irq->irq_pending_cnt++;
}

/* Unraise an interrupt */
void nvmet_mdev_irq_clear(struct nvmet_mdev_vctrl *vctrl, unsigned int index,
			  ktime_t now)
{
	struct nvmet_mdev_user_irq *irq = &vctrl->irqs.vecs[index];

	irq->irq_time = now;
	irq->irq_pending_cnt = 0;
}

/* Directly trigger an interrupt without affecting irq coalescing settings */
void nvmet_mdev_irq_trigger(struct nvmet_mdev_vctrl *vctrl, unsigned int index)
{
	struct nvmet_mdev_user_irq *irq = &vctrl->irqs.vecs[index];

	if (irq->trigger)
		eventfd_signal(irq->trigger);
}

/* Trigger previously raised interrupt */
void nvmet_mdev_irq_cond_trigger(struct nvmet_mdev_vctrl *vctrl,
				 unsigned int index, ktime_t now)
{
	struct nvmet_mdev_user_irq *irq = &vctrl->irqs.vecs[index];

	if (irq->irq_pending_cnt == 0)
		return;

	if (!nvmet_mdev_irq_coalesce(vctrl, irq, now)) {
		nvmet_mdev_irq_trigger(vctrl, index);
		nvmet_mdev_irq_clear(vctrl, index, now);
	}
}
