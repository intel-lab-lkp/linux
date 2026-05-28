// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "panthor_arbitration.h"
#include "panthor_device_io.h"
#include "panthor_partition_control.h"

#define AM_PART_IRQ_RAWSTAT			0x40
#define AM_PART_IRQ_CLEAR			0x44
#define AM_PART_IRQ_MASK			0x48
#define AM_PART_IRQ_STATUS			0x4C
#define   PART_RESET_DONE			BIT(3)
#define   PART_INVALID_COMMAND			BIT(6)

#define AM_PART_STATE				0x60
#define   AM_PART_STATE_GET(x)			FIELD_GET(GENMASK(11, 8), x)
#define     PART_STATE_RESET			0
#define     PART_STATE_WINDOW_OPENING		1
#define     PART_STATE_WINDOW_OPEN		7
#define     PART_STATE_WINDOW_CLOSED		8
#define   AM_PART_STATE_WINDOW_GET(x)		FIELD_GET(GENMASK(15, 12), x)

#define AM_PART_COMMAND				0x100
#define   AM_PART_SET_COMMAND(x)		FIELD_PREP(GENMASK(7, 0), x)
#define     PART_CMD_YIELD_IDLE			0x10
#define     PART_CMD_YIELD_NOW			0x11
#define     PART_CMD_CLOSE_WINDOW		0x20
#define     PART_CMD_OPEN_WINDOW		0x21
#define   AM_PART_SET_WINDOW(x)			FIELD_PREP(GENMASK(11, 8), x)

#define PART_CONTROL_IRQ_MASK \
	(PART_RESET_DONE | PART_INVALID_COMMAND)

#define PART_REG_POLL_SLEEP_US			10
#define PART_STATE_TRANSITION_TIMEOUT_US	5000000

/**
 * struct panthor_partition_control - Partition control data
 */
struct panthor_partition_control {
	/** @dev: Device pointer */
	struct device *dev;

	/** @name: Resource name */
	char *name;

	/** @iomem: CPU mapping of the partition control IOMEM region */
	void __iomem *iomem;

	/** @irq: IRQ number */
	int irq;

	/** @lock: Proctects partition control state data */
	spinlock_t lock;

	/** @current_aw: currently open access window */
	int current_aw;

	/** @closing: synchronous closing of the partition */
	bool closing;

	/** @waitqueue: Event wait queue. Not IRQ safe */
	wait_queue_head_t waitqueue;
};

static void partition_irq_suspend(struct panthor_partition_control *pc)
{
	gpu_write(pc->iomem, AM_PART_IRQ_MASK, 0);
}

static void partition_irq_resume(struct panthor_partition_control *pc)
{
	gpu_write(pc->iomem, AM_PART_IRQ_MASK, PART_CONTROL_IRQ_MASK);
}

static u32 partition_state_get(struct panthor_partition_control *pc)
{
	const u32 state = gpu_read(pc->iomem, AM_PART_STATE);

	return AM_PART_STATE_GET(state);
}

static u8 partition_aw_get(struct panthor_partition_control *pc)
{
	const u32 state = gpu_read(pc->iomem, AM_PART_STATE);

	return AM_PART_STATE_WINDOW_GET(state);
}

static int partition_state_wait(struct panthor_partition_control *pc, u32 state)
{
	u32 partition_state;

	return read_poll_timeout_atomic(partition_state_get, partition_state,
					partition_state == state,
					PART_REG_POLL_SLEEP_US,
					PART_STATE_TRANSITION_TIMEOUT_US,
					false, pc);
}

static int partition_state_wait_transition(struct panthor_partition_control *pc)
{
	int ret;

again:
	switch (partition_state_get(pc)) {
	case PART_STATE_WINDOW_CLOSED:
		ret = partition_state_wait(pc, PART_STATE_RESET);
		if (ret)
			return ret;
		goto again;
	case PART_STATE_WINDOW_OPENING:
		ret = partition_state_wait(pc, PART_STATE_WINDOW_OPEN);
		if (ret)
			return ret;
		goto again;
	case PART_STATE_RESET:
	case PART_STATE_WINDOW_OPEN:
		return 0;
	default:
		dev_err(pc->dev, "%s: Unknown state %u", pc->name,
			partition_state_get(pc));
		return -EINVAL;
	}
}

static int yield_now(struct panthor_partition_control *pc)
{
	int ret;

	ret = partition_state_wait_transition(pc);
	if (ret)
		return ret;

	/* Partition already reset, nothing to yield */
	if (partition_state_get(pc) == PART_STATE_RESET)
		return 0;

	gpu_write(pc->iomem, AM_PART_COMMAND,
		  AM_PART_SET_COMMAND(PART_CMD_YIELD_NOW));

	return 0;
}

static int window_close(struct panthor_partition_control *pc)
{
	int ret;

	ret = partition_state_wait_transition(pc);
	if (ret)
		return ret;

	/* Partition already closed. */
	if (partition_state_get(pc) == PART_STATE_RESET)
		return 0;

	scoped_guard(spinlock_irqsave, &pc->lock)
		pc->closing = true;

	gpu_write(pc->iomem, AM_PART_COMMAND,
		  AM_PART_SET_COMMAND(PART_CMD_CLOSE_WINDOW));

	/*
	 * Wait for RESET_DONE interrupt to clear pc->closing. Prevents stale
	 * RESET_DONE from being misinterpreted as RESET_DONE due to YIELD_DONE.
	 */
	if (!wait_event_timeout(
		    pc->waitqueue, !pc->closing,
		    usecs_to_jiffies(PART_STATE_TRANSITION_TIMEOUT_US))) {
		if (pc->closing) {
			dev_err(pc->dev,
				"%s: Timed out waiting for partition to close: %d",
				pc->name, ret);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

static int window_open(struct panthor_partition_control *pc, u8 aw_id)
{
	int ret;

	if (aw_id >= AM_ARB_MAX_AW_COUNT)
		return -EINVAL;

	ret = partition_state_wait_transition(pc);
	if (ret)
		return ret;

	if (partition_state_get(pc) == PART_STATE_WINDOW_OPEN) {
		dev_warn(pc->dev,
			 "%s: Window opem for %u Failed. Already opened. aw=%u, state=%u",
			 pc->name, aw_id, partition_aw_get(pc),
			 partition_state_get(pc));
		return 0;
	}

	gpu_write(pc->iomem, AM_PART_COMMAND,
		  AM_PART_SET_COMMAND(PART_CMD_OPEN_WINDOW) | AM_PART_SET_WINDOW(aw_id));

	ret = partition_state_wait(pc, PART_STATE_WINDOW_OPEN);
	if (ret) {
		dev_err(pc->dev, "%s: Timed out waiting for partition to open: %d",
			pc->name, ret);
		return ret;
	}

	scoped_guard(spinlock_irqsave, &pc->lock)
		pc->current_aw = aw_id;

	return 0;
}

static void partition_handle_reset_done(struct panthor_partition_control *pc)
{
	bool notify_stopped = false;
	int aw_id;

	scoped_guard(spinlock_irqsave, &pc->lock) {
		aw_id = pc->current_aw;
		pc->current_aw = -1;

		/* RESET_DONE from CLOSE_WINDOW */
		if (pc->closing)
			pc->closing = false;
		else if (aw_id >= 0)
			notify_stopped = true;
	}

	wake_up_all(&pc->waitqueue);

	if (notify_stopped)
		panthor_arbitration_on_stopped(dev_get_drvdata(pc->dev), aw_id);
}

static irqreturn_t partition_irq_raw_handler(int irq, void *data)
{
	struct panthor_partition_control *pc = data;
	u32 status;

	status = gpu_read(pc->iomem, AM_PART_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	if (status & PART_RESET_DONE)
		partition_handle_reset_done(pc);

	if (status & PART_INVALID_COMMAND)
		dev_warn(pc->dev, "%s: Invalid command", pc->name);

	gpu_write(pc->iomem, AM_PART_IRQ_CLEAR, status);

	return IRQ_HANDLED;
}

int panthor_partition_control_suspend(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		struct panthor_partition_control *pc = adev->pc[i];

		if (!pc)
			continue;

		partition_irq_suspend(pc);
	}

	return 0;
}

int panthor_partition_control_resume(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		struct panthor_partition_control *pc = adev->pc[i];

		if (!pc)
			continue;

		partition_irq_resume(pc);
	}

	return 0;
}

void panthor_partition_control_term(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		struct panthor_partition_control *pc = adev->pc[i];

		if (!pc)
			continue;

		if (!WARN_ON(window_close(pc)))
			WARN_ON(partition_state_wait(pc, PART_STATE_RESET));

		partition_irq_suspend(pc);
	}
}

static int partition_control_init(struct panthor_arbitration *adev, int i)
{
	struct device *dev = adev->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct panthor_partition_control *pc;
	struct resource *res;
	void __iomem *iomem;
	char *name;
	int irq;
	int ret;

	if (i >= AM_ARB_MAX_PC_COUNT)
		return -EINVAL;

	name = devm_kasprintf(dev, GFP_KERNEL, "pc%d", i);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res) {
		devm_kfree(dev, name);
		return -ENODEV;
	}

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	iomem = devm_ioremap_resource(dev, res);
	if (IS_ERR(iomem)) {
		return dev_err_probe(dev, PTR_ERR(iomem),
				     "%s: Failed to ioremap", name);
	}

	pc->name = name;
	pc->dev = dev;
	pc->iomem = iomem;

	init_waitqueue_head(&pc->waitqueue);
	spin_lock_init(&pc->lock);
	pc->current_aw = -1;

	adev->pc[i] = pc;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(
		dev, irq, partition_irq_raw_handler, IRQF_SHARED,
		devm_kasprintf(dev, GFP_KERNEL, "panthor-%s-irq", name), pc);
	if (ret)
		return ret;

	partition_irq_resume(pc);

	return 0;
}

int panthor_partition_control_init(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_PC_COUNT; i++) {
		int ret = partition_control_init(adev, i);
		if (ret == -ENODEV)
			continue;

		if (ret)
			return ret;
	}

	return 0;
}

int panthor_partition_control_open_window(struct panthor_partition_control *pc, u8 aw_id)
{
	return window_open(pc, aw_id);
}

int panthor_partition_control_close_window(struct panthor_partition_control *pc)
{
	return window_close(pc);
}

int panthor_partition_control_yield_now(struct panthor_partition_control *pc)
{
	return yield_now(pc);
}
