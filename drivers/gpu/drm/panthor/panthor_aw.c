// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/platform_device.h>

#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#include <drm/drm_drv.h>

#include "panthor_am_msg.h"
#include "panthor_aw.h"
#include "panthor_device.h"
#include "panthor_fw.h"
#include "panthor_hw.h"
#include "panthor_sched.h"

#include "panthor_trace.h"

#define WINDOW_CONTROL_BASE				0x0
#define WINDOW_CONTROL_SIZE				0x1000

#define WINDOW_STATUS					0x43c
#define   WINDOW_STATUS_IRQ_WINDOW_CONTROL		BIT(0)
#define   WINDOW_STATUS_IRQ_GPU_CONTROL			BIT(1)
#define   WINDOW_STATUS_IRQ_GPU_POWER			BIT(2)
#define   WINDOW_STATUS_IRQ_JOB_CONTROL			BIT(3)
#define   WINDOW_STATUS_IRQ_MMU_CONTROL			BIT(4)
#define   WINDOW_STATUS_WINDOW_OPEN			BIT(31)

#define WINDOW_INT_BASE					0x440
#define   WINDOW_IRQ_MESSAGE				BIT(0)
#define   WINDOW_IRQ_INVALID_ACCESS			BIT(1)
#define   WINDOW_IRQ_WINDOW_OPENING			BIT(2)
#define   WINDOW_IRQ_WINDOW_OPENED			BIT(3)
#define   WINDOW_IRQ_WINDOW_CLOSED			BIT(4)

#define WINDOW_MESSAGE_BASE				0x460

#define WINDOW_IRQ_MASK \
	(WINDOW_IRQ_MESSAGE | WINDOW_IRQ_WINDOW_OPENED | WINDOW_IRQ_WINDOW_CLOSED)

#define PANTHOR_AW_HANDSHAKE_TIMEOUT_MS			(5 * MSEC_PER_SEC)
#define PANTHOR_AW_GPU_REQUEST_TIMEOUT_MS		(5 * MSEC_PER_SEC)
#define PANTHOR_AW_STATE_TRANSITION_TIMEOUT_MS		(5 * MSEC_PER_SEC)

#define panthor_aw_wait_cond(_aw, _cond, _timeout_ms)				\
({										\
	int __ret = 0;								\
	if (!wait_event_timeout((_aw)->waitqueue, _cond,			\
				msecs_to_jiffies(_timeout_ms)))			\
		if (!(_cond))							\
			__ret = -ETIMEDOUT;					\
	__ret;									\
})

struct panthor_aw {
	/** @ptdev: Pointer to Panthor device */
	struct panthor_device *ptdev;

	/** @iomem: CPU mapping of WINDOW_CONTROL iomem region */
	void __iomem *iomem;

	/** @irq: IRQ data */
	struct panthor_irq irq;

	/** @msg: Messaging data */
	struct panthor_am_msg msg;

	/** @waitqueue: Event wait queue. Not IRQ safe */
	wait_queue_head_t waitqueue;

	/** @state: Current state of the AW */
	atomic_t state;

	/** @wq: Scheduler workqueue to dispatch events */
	struct workqueue_struct *wq;

	/** @msg_retry_work: Work to resend messages */
	struct work_struct msg_retry_work;
};

static bool panthor_aw_is_open(struct panthor_aw *aw)
{
	return gpu_read(aw->iomem, WINDOW_STATUS) & WINDOW_STATUS_WINDOW_OPEN;
}

static void panthor_aw_state_set(struct panthor_aw *aw, enum aw_states new)
{
	atomic_set(&aw->state, new);

	wake_up_all(&aw->waitqueue);
}

static bool panthor_aw_state_try_set(struct panthor_aw *aw,
				     enum aw_states expected,
				     enum aw_states new)
{
	int old_state = atomic_cmpxchg(&aw->state, expected, new);
	bool res = (old_state == expected);

	if (res)
		wake_up_all(&aw->waitqueue);

	return res;
}

static int panthor_aw_state_wait(struct panthor_aw *aw, enum aw_states target, u32 timeout_ms)
{
	return panthor_aw_wait_cond(aw, atomic_read(&aw->state) == target, timeout_ms);
}

static int panthor_aw_state_wait_transition(struct panthor_aw *aw, u32 timeout_ms)
{
	return panthor_aw_wait_cond(
		aw,
		(atomic_read(&aw->state) == PANTHOR_AW_STATE_READY ||
		 atomic_read(&aw->state) == PANTHOR_AW_STATE_GPU_GRANTED),
		timeout_ms);
}

static void panthor_aw_msg_retry_work(struct work_struct *work)
{
	struct panthor_aw *aw =
		container_of(work, struct panthor_aw, msg_retry_work);
	struct panthor_am_msg *msg = &aw->msg;
	int ret;

	ret = panthor_am_msg_retry(msg);
	if (ret == -EINVAL)
		drm_warn(&aw->ptdev->base, "Send FIFO unexpectedly empty");

	if (ret == -EBUSY || ret == -EAGAIN)
		queue_work(aw->wq, &aw->msg_retry_work);
}

static void panthor_aw_send_msg(struct panthor_aw *aw, u64 message)
{
	struct panthor_device *ptdev = aw->ptdev;
	int ret;

	ret = panthor_am_msg_send(&aw->msg, message);
	if (ret == -ENOMEM)
		drm_err(&ptdev->base, "Send FIFO is full");

	if (ret == -EBUSY) {
		drm_dbg(&ptdev->base, "Pending messages, scheduling retry work");
		queue_work(aw->wq, &aw->msg_retry_work);
	}
}

static void panthor_aw_handshake_handle(struct panthor_aw *aw, u64 message)
{
	struct panthor_device *ptdev = aw->ptdev;
	bool acked = AM_MSG_ACK_GET(message);
	u8 version = AM_MSG_VERSION_GET(message);
	int ret;

	ret = panthor_am_msg_version_validate(&aw->msg, version);
	if (ret == -EOPNOTSUPP)
		drm_warn(&ptdev->base,
			 "Msg protocol version less than minimum supported (%u < %u)",
			 version, AM_MSG_MIN_SUPPORTED_VERSION);

	if (!acked) {
		u64 reply = VM_ARB_INIT_MAKE(1, aw->msg.version);

		panthor_aw_send_msg(aw, reply);

		/* TODO: Handle AW restart */
	}

	panthor_aw_state_try_set(aw, PANTHOR_AW_STATE_INIT, PANTHOR_AW_STATE_READY);
}

static void panthor_aw_handshake_init(struct panthor_aw *aw)
{
	u64 message = VM_ARB_INIT_MAKE(0, AM_MSG_CURRENT_VERSION);

	panthor_aw_send_msg(aw, message);
}

static void panthor_aw_handle_message(struct panthor_aw *aw)
{
	struct panthor_device *ptdev = aw->ptdev;
	const u64 message = panthor_am_msg_read(&aw->msg);
	const u8 msg_id = AM_MSG_ID_GET(message);

	/* currently only support ARB_VM_INIT */
	if (msg_id == ARB_VM_INIT)
		panthor_aw_handshake_handle(aw, message);
	else
		drm_warn(&ptdev->base, "Unsupported msg id (0x%x)", msg_id);
}

static irqreturn_t panthor_aw_irq_raw_hander(int irq, void *data)
{
	struct panthor_irq *pirq = data;
	struct panthor_device *ptdev = pirq->ptdev;
	struct panthor_aw *aw = ptdev->aw;
	u32 status;

	scoped_guard(spinlock_irqsave, &pirq->mask_lock) {
		if (atomic_read(&pirq->state) != PANTHOR_IRQ_STATE_ACTIVE)
			return IRQ_NONE;
	}

	status = gpu_read(pirq->iomem, INT_STAT);
	if (!status)
		return IRQ_NONE;

	if (status & WINDOW_IRQ_MESSAGE)
		panthor_aw_handle_message(aw);

	/* TODO: handle WINDOW_CLOSED*/

	if ((status & WINDOW_IRQ_WINDOW_OPENED) && panthor_aw_is_open(aw))
		panthor_aw_state_try_set(aw, PANTHOR_AW_STATE_GPU_REQUEST,
					 PANTHOR_AW_STATE_GPU_GRANTED);

	gpu_write(pirq->iomem, INT_CLEAR, status);

	return IRQ_HANDLED;
}

static void panthor_aw_irq_suspend(struct panthor_irq *pirq)
{
	scoped_guard(spinlock_irqsave, &pirq->mask_lock) {
		atomic_set(&pirq->state, PANTHOR_IRQ_STATE_SUSPENDING);
		gpu_write(pirq->iomem, INT_MASK, 0);
	}
	synchronize_irq(pirq->irq);
	atomic_set(&pirq->state, PANTHOR_IRQ_STATE_SUSPENDED);
}

static void panthor_aw_irq_resume(struct panthor_irq *pirq)
{
	guard(spinlock_irqsave)(&pirq->mask_lock);

	atomic_set(&pirq->state, PANTHOR_IRQ_STATE_ACTIVE);
	gpu_write(pirq->iomem, INT_MASK, pirq->mask);
}

static int panthor_request_aw_irq(struct panthor_device *ptdev,
				  struct panthor_irq *pirq, int irq, u32 mask,
				  void __iomem *iomem)
{
	int ret;

	pirq->ptdev = ptdev;
	pirq->irq = irq;
	pirq->mask = mask;
	pirq->iomem = iomem;
	spin_lock_init(&pirq->mask_lock);

	ret = devm_request_irq(ptdev->base.dev, irq, panthor_aw_irq_raw_hander,
			       IRQF_SHARED, "panthor-aw", pirq);
	if (ret)
		return ret;

	panthor_aw_irq_resume(pirq);

	return 0;
}

static int panthor_aw_request(struct panthor_aw *aw)
{
	int ret;

again:
	switch(atomic_read(&aw->state)) {
	case PANTHOR_AW_STATE_GPU_GRANTED:
		return 0;

	case PANTHOR_AW_STATE_READY:
		break;

	case PANTHOR_AW_STATE_INIT:
		panthor_aw_handshake_init(aw);
		ret = panthor_aw_state_wait(aw, PANTHOR_AW_STATE_READY,
					    PANTHOR_AW_HANDSHAKE_TIMEOUT_MS);
		if (ret)
			return ret;
		goto again;
	default:
		ret = panthor_aw_state_wait_transition(
			aw, PANTHOR_AW_STATE_TRANSITION_TIMEOUT_MS);
		if (ret)
			return ret;
		goto again;
	}

	if (!panthor_aw_state_try_set(aw, PANTHOR_AW_STATE_READY,
				      PANTHOR_AW_STATE_GPU_REQUEST))
		goto again;

	panthor_aw_send_msg(aw, VM_ARB_GPU_REQUEST);

	/* Wait for GPU to be granted */
	ret = panthor_aw_state_wait(aw, PANTHOR_AW_STATE_GPU_GRANTED,
				    PANTHOR_AW_GPU_REQUEST_TIMEOUT_MS);
	if (ret) {
		panthor_aw_state_set(aw, PANTHOR_AW_STATE_READY);
		return ret;
	}

	return 0;
}

static int panthor_aw_yield(struct panthor_aw *aw)
{
	panthor_aw_send_msg(aw, VM_ARB_GPU_STOPPED);

	return panthor_aw_state_wait(aw, PANTHOR_AW_STATE_READY,
				     PANTHOR_AW_STATE_TRANSITION_TIMEOUT_MS);
}

int panthor_aw_init(struct panthor_device *ptdev)
{
	struct panthor_aw *aw;
	int irq;
	int ret;

	if (!panthor_hw_has_gpu_discover(ptdev))
		return 0;

	aw = drmm_kzalloc(&ptdev->base, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;

	aw->wq = drmm_alloc_ordered_workqueue(&ptdev->base, "panthor-aw-wq", 0);
	if (!aw->wq)
		return -ENOMEM;

	aw->ptdev = ptdev;
	aw->iomem = ptdev->iomem;

	INIT_WORK(&aw->msg_retry_work, panthor_aw_msg_retry_work);

	init_waitqueue_head(&aw->waitqueue);
	atomic_set(&aw->state, PANTHOR_AW_STATE_INIT);

	panthor_am_msg_init(&aw->msg, aw->iomem + WINDOW_MESSAGE_BASE);

	ptdev->aw = aw;

	irq = platform_get_irq_byname(to_platform_device(ptdev->base.dev), "gpu");
	if (irq < 0)
		return irq;

	ret = panthor_request_aw_irq(ptdev, &aw->irq, irq, WINDOW_IRQ_MASK,
				     aw->iomem + WINDOW_INT_BASE);
	if (ret)
		return ret;

	ret = panthor_aw_request(aw);
	if (ret)
		return ret;

	return 0;
}

void panthor_aw_unplug(struct panthor_device *ptdev)
{
	struct panthor_aw *aw = ptdev->aw;

	if (!aw)
		return;

	disable_work_sync(&aw->msg_retry_work);

	panthor_aw_irq_suspend(&aw->irq);
}

int panthor_aw_resume(struct panthor_device *ptdev)
{
	struct panthor_aw *aw = ptdev->aw;
	int ret;

	if (!aw)
		return 0;

	panthor_aw_irq_resume(&aw->irq);

	ret = panthor_aw_request(aw);
	if (ret) {
		drm_warn(&ptdev->base, "Timedout waiting for GPU to be granted");
		goto err_out;
	}

	return 0;

err_out:
	panthor_aw_irq_suspend(&aw->irq);
	return ret;
}

int panthor_aw_suspend(struct panthor_device *ptdev)
{
	struct panthor_aw *aw = ptdev->aw;
	int ret = 0;

	/* suspend hw components directly if AW is not supported */
	if (!aw)
		return 0;

	if (atomic_read(&aw->state) == PANTHOR_AW_STATE_READY)
		goto out_irq_suspend;

	if (panthor_aw_state_try_set(aw, PANTHOR_AW_STATE_GPU_GRANTED,
		PANTHOR_AW_STATE_GPU_STOPPED))
		ret = panthor_aw_yield(aw);
	else
		ret = panthor_aw_state_wait(
			aw, PANTHOR_AW_STATE_READY,
			PANTHOR_AW_STATE_TRANSITION_TIMEOUT_MS);

out_irq_suspend:
	panthor_aw_irq_suspend(&aw->irq);
	return ret;
}
