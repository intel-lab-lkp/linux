// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/device.h>
#include <linux/kfifo.h>
#include <linux/types.h>
#include <linux/time.h>

#include "panthor_arbitration.h"
#include "panthor_arbitration_sched.h"
#include "panthor_resource_group.h"

static u64 grant_timeout_us = 1 * USEC_PER_SEC;
static u64 yield_timeout_us = 1 * USEC_PER_SEC;

module_param(grant_timeout_us, ullong, 0664);
MODULE_PARM_DESC(grant_timeout_us, "Time the GPU will be granted to the AW in microseconds.");

module_param(yield_timeout_us, ullong, 0664);
MODULE_PARM_DESC(yield_timeout_us, "Time, in microseconds, within which the AW must yeild the GPU.");

enum arbitration_sched_phase {
	ARB_SCHED_PHASE_IDLE = 0,
	ARB_SCHED_PHASE_GRANTING,
	ARB_SCHED_PHASE_GRANTED,
	ARB_SCHED_PHASE_STOPPING,
	ARB_SCHED_PHASE_LOST,
};

struct panthor_arbitration_timer {
	/** @timer: High-resolution timer */
	struct hrtimer timer;

	/** @epoch: The lease generation for which the timer was triggered */
	u32 epoch;
};

struct panthor_arbitration_work {
	/** @work: Work struct */
	struct work_struct work;

	/** @aw_id: Window for which the work was queued */
	u8 aw_id;

	/** @epoch: The lease generation for which the work was queued */
	u32 epoch;
};

struct panthor_arbitration_sched {
	/** @dev: Device pointer */
	struct device *dev;

	/** @name: Scheduler indentifying name */
	char *name;

	/** @waitqueue: Event wait queue. Not IRQ safe */
	wait_queue_head_t waitqueue;

	/** @lock: Protects scheduler state */
	spinlock_t lock;

	/** @queue: Pending request FIFO */
	DECLARE_KFIFO(queue, u8, 16);

	/** @queued_mask: Mask of currently queued AWs */
	DECLARE_BITMAP(queued_mask, AM_ARB_MAX_AW_COUNT);

	/** @active_aw_id: Currently active AW. Can be -1 */
	s8 active_aw_id;

	/** @phase: Current scheduling phase */
	enum arbitration_sched_phase phase;

	/** @epoch: Current active lease generation */
	u32 epoch;

	/** @wq: Scheduler workqueue to dispatch events */
	struct workqueue_struct *wq;

	/** @grant_work: Work to grant the GPU to the AW */
	struct panthor_arbitration_work grant_work;

	/** @stop_work: Work to hard-close the AW */
	struct panthor_arbitration_work stop_work;

	/** @close_work: Work to hard-close the AW */
	struct panthor_arbitration_work close_work;

	/** @grant_timer: Timer to indicate grant window has lapsed */
	struct panthor_arbitration_timer grant_timer;

	/** @stop_timer: Timer to indicate yield window has lapsed */
	struct panthor_arbitration_timer stop_timer;

	/** @disabled: Scheduler disabled. No further scheduling can happen */
	bool disabled;

	/**
	 * @grant_previously_expired: Grant period previously expired but was
	 * not asked to stop
	 */
	bool grant_previously_expired;
};

static inline struct panthor_arbitration *
to_adev(struct panthor_arbitration_sched *sched)
{
	return dev_get_drvdata(sched->dev);
}

static inline struct panthor_arbitration_work *to_work(struct work_struct *w)
{
	return container_of(w, struct panthor_arbitration_work, work);
}

static bool arb_sched_phase_reached(struct panthor_arbitration_sched *sched,
				    enum arbitration_sched_phase phase)
{
	guard(spinlock_irqsave)(&sched->lock);

	return sched->phase == phase;
}

/**
 * sched_wait_phase - Wait for scheduler to enter a phase
 *
 * This method is not IRQ safe as it sleeps.
 */
static int arb_sched_wait_phase(struct panthor_arbitration_sched *sched,
				enum arbitration_sched_phase phase,
				u64 timeout_us)
{
	if (!wait_event_timeout(
		    sched->waitqueue, arb_sched_phase_reached(sched, phase),
		    usecs_to_jiffies(timeout_us))) {

		if (!arb_sched_phase_reached(sched, phase))
			return -ETIMEDOUT;
	}

	return 0;
}

static void arb_sched_disable(struct panthor_arbitration_sched *sched)
{
	scoped_guard(spinlock_irqsave, &sched->lock)
		sched->disabled = true;

	hrtimer_cancel(&sched->grant_timer.timer);
}

static void arb_sched_enable(struct panthor_arbitration_sched *sched)
{
	scoped_guard(spinlock_irqsave, &sched->lock)
		sched->disabled = false;
}

static void arb_sched_queue_work(struct panthor_arbitration_sched *sched,
				 struct panthor_arbitration_work *work)
{
	lockdep_assert_held(&sched->lock);

	work->aw_id = sched->active_aw_id;
	work->epoch = sched->epoch;
	queue_work(sched->wq, &work->work);
}

static void arb_sched_request_stop_locked(struct panthor_arbitration_sched *sched)
{
	lockdep_assert_held(&sched->lock);

	if (sched->phase != ARB_SCHED_PHASE_GRANTED)
		return;

	if (sched->active_aw_id < 0)
		return;

	/*
	 * In the event of a single requesting AW, let it continue to
	 * lease the GPU until another request comes in.
	 */
	if (!sched->disabled && kfifo_is_empty(&sched->queue)) {
		sched->grant_previously_expired = true;
		return;
	}

	sched->phase = ARB_SCHED_PHASE_STOPPING;
	arb_sched_queue_work(sched, &sched->stop_work);

	wake_up_all(&sched->waitqueue);
}

static void arb_sched_request_stop(struct panthor_arbitration_sched *sched)
{
	guard(spinlock_irqsave)(&sched->lock);

	arb_sched_request_stop_locked(sched);
}

static void arb_sched_force_close_locked(struct panthor_arbitration_sched *sched)
{
	lockdep_assert_held(&sched->lock);

	if (sched->phase == ARB_SCHED_PHASE_IDLE ||
	    sched->phase == ARB_SCHED_PHASE_LOST)
		return;

	if (sched->active_aw_id < 0)
		return;

	sched->phase = ARB_SCHED_PHASE_LOST;

	arb_sched_queue_work(sched, &sched->close_work);

	wake_up_all(&sched->waitqueue);
}

static void arb_sched_force_close(struct panthor_arbitration_sched *sched)
{
	guard(spinlock_irqsave)(&sched->lock);

	arb_sched_force_close_locked(sched);
}

static int arb_sched_next_locked(struct panthor_arbitration_sched *sched)
{
	u8 aw_id;

	lockdep_assert_held(&sched->lock);

	if (sched->disabled)
		return 0;

	/* already have a running aw */
	if (sched->active_aw_id >= 0)
		return 0;

	/* nothing to schedule */
	if (kfifo_is_empty(&sched->queue))
		return 0;

	if (!kfifo_get(&sched->queue, &aw_id)) {
		dev_warn(sched->dev, "%s: queue unexpectedly empty",
				sched->name);
		return -EINVAL;
	}

	clear_bit(aw_id, sched->queued_mask);

	sched->active_aw_id = aw_id;
	sched->epoch++;
	sched->phase = ARB_SCHED_PHASE_GRANTING;

	arb_sched_queue_work(sched, &sched->grant_work);

	wake_up_all(&sched->waitqueue);

	return 0;
}

static int arb_sched_next(struct panthor_arbitration_sched *sched)
{
	guard(spinlock_irqsave)(&sched->lock);

	return arb_sched_next_locked(sched);
}

static void arb_sched_reset(struct panthor_arbitration_sched *sched,
				    u32 epoch, u8 aw_id)
{
	scoped_guard(spinlock_irqsave, &sched->lock) {
		if (sched->epoch != epoch)
			return;

		if (sched->active_aw_id != aw_id)
			return;

		dev_warn(sched->dev, "%s: scheduler reset. active_aw=%u",
			 sched->name, sched->active_aw_id);

		sched->epoch++;
		sched->active_aw_id = -1;
		sched->phase = ARB_SCHED_PHASE_IDLE;
		sched->grant_previously_expired = false;

		wake_up_all(&sched->waitqueue);
	}

	hrtimer_cancel(&sched->grant_timer.timer);
	hrtimer_cancel(&sched->stop_timer.timer);

	arb_sched_next(sched);
}

static void arb_sched_grant_work(struct work_struct *work)
{
	struct panthor_arbitration_work *w = to_work(work);
	struct panthor_arbitration_sched *sched =
		container_of(w, struct panthor_arbitration_sched, grant_work);
	int ret;
	u32 epoch;
	u8 aw_id;

	scoped_guard(spinlock_irqsave, &sched->lock) {
		if (sched->epoch != w->epoch)
			return;

		if (sched->active_aw_id != w->aw_id)
			return;

		epoch = sched->epoch;
		aw_id = sched->active_aw_id;
	}

	ret = panthor_arbitration_on_grant(to_adev(sched), aw_id);
	if (!ret) {
		guard(spinlock_irqsave)(&sched->lock);

		if (sched->phase != ARB_SCHED_PHASE_GRANTING) {
			dev_warn(sched->dev, "%s: Grant ACKED when not granting",
				 sched->name);
			return;
		}

		if (sched->active_aw_id != aw_id)
			return;

		if (sched->disabled) {
			sched->phase = ARB_SCHED_PHASE_STOPPING;
			arb_sched_queue_work(sched, &sched->stop_work);
		} else {
			sched->phase = ARB_SCHED_PHASE_GRANTED;

			sched->grant_timer.epoch = sched->epoch;
			hrtimer_start(&sched->grant_timer.timer,
				      us_to_ktime(grant_timeout_us),
				      HRTIMER_MODE_REL);
		}

		wake_up_all(&sched->waitqueue);
	} else {
		dev_err(sched->dev, "%s: Failed to grant access to AW%u",
			sched->name, aw_id);

		arb_sched_reset(sched, epoch, aw_id);
	}
}

static void arb_sched_stop_work(struct work_struct *work)
{
	struct panthor_arbitration_work *w = to_work(work);
	struct panthor_arbitration_sched *sched =
		container_of(w, struct panthor_arbitration_sched, stop_work);
	int ret;
	u32 epoch;
	u8 aw_id;

	scoped_guard(spinlock_irqsave, &sched->lock) {
		if (sched->epoch != w->epoch)
			return;

		if (sched->active_aw_id != w->aw_id)
			return;

		epoch = sched->epoch;
		aw_id = sched->active_aw_id;
	}

	ret = panthor_arbitration_on_stop(to_adev(sched), aw_id);
	if (!ret) {
		guard(spinlock_irqsave)(&sched->lock);

		if (sched->phase != ARB_SCHED_PHASE_STOPPING) {
			dev_warn(sched->dev, "%s: Stop ACKED when not yielding",
				 sched->name);
			return;
		}

		if (sched->active_aw_id != aw_id)
			return;

		sched->stop_timer.epoch = sched->epoch;
		hrtimer_start(&sched->stop_timer.timer,
			      us_to_ktime(yield_timeout_us), HRTIMER_MODE_REL);
	} else {
		dev_err(sched->dev, "%s: Failed to send yield to AW%u",
			sched->name, aw_id);

		arb_sched_reset(sched, epoch, aw_id);
	}
}

static void arb_sched_close_work(struct work_struct *work)
{
	struct panthor_arbitration_work *w = to_work(work);
	struct panthor_arbitration_sched *sched =
		container_of(w, struct panthor_arbitration_sched, close_work);
	int ret;
	u32 epoch;
	u8 aw_id;

	scoped_guard(spinlock_irqsave, &sched->lock) {
		if (sched->epoch != w->epoch)
			return;

		if (sched->active_aw_id != w->aw_id)
			return;

		epoch = sched->epoch;
		aw_id = sched->active_aw_id;
	}

	ret = panthor_arbitration_on_close(to_adev(sched), w->aw_id);
	if (ret) {
		dev_err(sched->dev, "%s: Failed to close AW%u",
			sched->name, aw_id);
		arb_sched_reset(sched, epoch, aw_id);
	} else {
		panthor_arbitration_sched_on_stopped(sched, w->aw_id);
	}
}

static enum hrtimer_restart
panthor_arbitration_sched_on_stop_timeout(struct hrtimer *timer)
{
	struct panthor_arbitration_timer *atmr = container_of(
		timer, struct panthor_arbitration_timer, timer);
	struct panthor_arbitration_sched *sched = container_of(
		atmr, struct panthor_arbitration_sched, stop_timer);

	guard(spinlock_irqsave)(&sched->lock);

	if (sched->epoch != atmr->epoch)
		return HRTIMER_NORESTART;

	dev_warn(sched->dev,
		 "%s: AW%d failed to stop in time. Force closing...",
		 sched->name, sched->active_aw_id);
	arb_sched_force_close_locked(sched);

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart
panthor_arbitration_sched_on_grant_timeout(struct hrtimer *timer)
{
	struct panthor_arbitration_timer *atmr = container_of(
		timer, struct panthor_arbitration_timer, timer);
	struct panthor_arbitration_sched *sched = container_of(
		atmr, struct panthor_arbitration_sched, grant_timer);

	guard(spinlock_irqsave)(&sched->lock);

	if (sched->epoch != atmr->epoch)
		return HRTIMER_NORESTART;

	arb_sched_request_stop_locked(sched);

	return HRTIMER_NORESTART;
}

int panthor_arbitration_sched_on_request(struct panthor_arbitration_sched *sched, u8 aw_id)
{
	guard(spinlock_irqsave)(&sched->lock);

	if (aw_id >= AM_ARB_MAX_AW_COUNT)
		return -EINVAL;

	if (test_bit(aw_id, sched->queued_mask) ||
	    (sched->active_aw_id == aw_id &&
	     sched->phase != ARB_SCHED_PHASE_STOPPING)) {
		dev_warn(sched->dev, "%s: AW%u already in request queue.",
			 sched->name, aw_id);
		return -EEXIST;
	}

	/* Always put onto queue */
	kfifo_put(&sched->queue, aw_id);
	set_bit(aw_id, sched->queued_mask);

	/* Already have running or pending, exit. */
	if (sched->active_aw_id >= 0) {
		/*
		 * Grant timer for the current AW has previously expired,
		 * request it to yield the GPU.
		 */
		if (sched->grant_previously_expired)
			arb_sched_request_stop_locked(sched);

		return 0;
	}

	return arb_sched_next_locked(sched);
}

int panthor_arbitration_sched_on_idle(struct panthor_arbitration_sched *sched, u8 aw_id)
{
	guard(spinlock_irqsave)(&sched->lock);

	if (aw_id >= AM_ARB_MAX_AW_COUNT)
		return -EINVAL;

	if (sched->active_aw_id != aw_id)
		return 0;

	if (sched->phase != ARB_SCHED_PHASE_GRANTED)
		return 0;

	sched->phase = ARB_SCHED_PHASE_STOPPING;
	arb_sched_queue_work(sched, &sched->close_work);

	return 0;
}

int panthor_arbitration_sched_on_stopped(struct panthor_arbitration_sched *sched, u8 aw_id)
{
	guard(spinlock_irqsave)(&sched->lock);

	if (aw_id >= AM_ARB_MAX_AW_COUNT)
		return -EINVAL;

	if (sched->phase == ARB_SCHED_PHASE_IDLE)
		return 0;

	if (sched->active_aw_id != aw_id)
		return 0;

	/* retire old lease */
	sched->epoch++;

	sched->grant_previously_expired = false;
	sched->active_aw_id = -1;
	sched->phase = ARB_SCHED_PHASE_IDLE;

	wake_up_all(&sched->waitqueue);

	return arb_sched_next_locked(sched);
}

static void arbitration_sched_term(struct panthor_arbitration_sched *sched)
{
	WARN_ON(panthor_arbitration_sched_stop(sched));

	disable_work_sync(&sched->grant_work.work);
	disable_work_sync(&sched->stop_work.work);
	disable_work_sync(&sched->close_work.work);
}

static int arbitration_sched_init(struct panthor_arbitration *adev, int i)
{
	struct device *dev = adev->dev;
	struct panthor_arbitration_sched *sched;

	sched = devm_kzalloc(dev, sizeof(*sched), GFP_KERNEL);
	if (!sched)
		return -ENOMEM;

	sched->name = devm_kasprintf(dev, GFP_KERNEL, "sched%d", i);
	sched->dev = dev;

	sched->wq = devm_alloc_ordered_workqueue(dev, "%s-wq", 0, sched->name);
	if (!sched->wq)
		return -ENOMEM;

	INIT_WORK(&sched->grant_work.work, arb_sched_grant_work);
	INIT_WORK(&sched->stop_work.work, arb_sched_stop_work);
	INIT_WORK(&sched->close_work.work, arb_sched_close_work);

	init_waitqueue_head(&sched->waitqueue);

	spin_lock_init(&sched->lock);
	INIT_KFIFO(sched->queue);
	sched->active_aw_id = -1;
	sched->phase = ARB_SCHED_PHASE_IDLE;

	hrtimer_setup(&sched->grant_timer.timer,
		      panthor_arbitration_sched_on_grant_timeout,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_setup(&sched->stop_timer.timer,
		      panthor_arbitration_sched_on_stop_timeout,
		      CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	adev->sched[i] = sched;

	return 0;
}

void panthor_arbitration_sched_term(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		struct panthor_arbitration_sched *sched = adev->sched[i];

		if (!sched)
			continue;

		arbitration_sched_term(sched);
	}
}

int panthor_arbitration_sched_init(struct panthor_arbitration *adev)
{

	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		int ret = arbitration_sched_init(adev, i);

		if (ret == -ENODEV)
			continue;

		if (ret)
			return ret;
	}

	return 0;
}

int panthor_arbitration_sched_suspend(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		struct panthor_arbitration_sched *sched = adev->sched[i];
		int ret;

		if (!sched)
			continue;

		ret = panthor_arbitration_sched_stop(sched);
		if (ret)
			return ret;
	}

	return 0;
}

int panthor_arbitration_sched_resume(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		struct panthor_arbitration_sched *sched = adev->sched[i];
		int ret;

		if (!sched)
			continue;

		ret = panthor_arbitration_sched_start(sched);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * panthor_arbitration_sched_stop - Stop active aw and pause scheduler
 *
 * This method is not IRQ safe as it waits for on_stopped() to be called.
 */
int panthor_arbitration_sched_stop(struct panthor_arbitration_sched *sched)
{
	int ret;

	arb_sched_disable(sched);

	arb_sched_request_stop(sched);

	/*
	 * Provisionally waiting for up to 2x yield_timeout, but path may
	 * include time taken for panthor_arbitration_on_grant() to complete
	 * if sched_request_stop() is called while in the GRANTING state.
	 *
	 * Realistically, if granting takes more than yield_timeout, HW is in
	 * bad state either way. Best continue and stop the scheduler.
	 */
	ret = arb_sched_wait_phase(sched, ARB_SCHED_PHASE_IDLE,
				   grant_timeout_us + yield_timeout_us);
	if (ret)
		arb_sched_force_close(sched);

	/* Should no longer have any further activity. Cancel timer. */
	hrtimer_cancel(&sched->stop_timer.timer);
	flush_workqueue(sched->wq);

	scoped_guard(spinlock_irqsave, &sched->lock)
		if (sched->phase == ARB_SCHED_PHASE_IDLE)
			return 0;

	return ret;
}

int panthor_arbitration_sched_start(struct panthor_arbitration_sched *sched)
{
	arb_sched_enable(sched);

	return arb_sched_next(sched);
}
