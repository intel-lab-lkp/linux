// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>

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
#define   AM_PART_GET_COMMAND(x)		FIELD_GET(GENMASK(7, 0), x)
#define     PART_CMD_YIELD_IDLE			0x10
#define     PART_CMD_YIELD_NOW			0x11
#define     PART_CMD_CLOSE_WINDOW		0x20
#define     PART_CMD_OPEN_WINDOW		0x21
#define   AM_PART_SET_WINDOW(x)			FIELD_PREP(GENMASK(11, 8), x)

#define PART_CONTROL_IRQ_MASK \
	(PART_RESET_DONE | PART_INVALID_COMMAND)

#define PART_REG_POLL_SLEEP_US			10
#define PART_STATE_TRANSITION_TIMEOUT_US	5000000

/** struct panthor_partition_cmd - Partition command data */
struct panthor_partition_cmd {
	/** @cmd: AM_PARTITION_COMMAND value */
	u32 cmd;

	/** @aw_id: AW ID the command was sent for */
	int aw_id;
};

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

	/** @last_cmd: Last sent command */
	struct panthor_partition_cmd last_cmd;
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

static inline void
partition_command_write_untracked(struct panthor_partition_control *pc, u32 cmd)
{
	gpu_write(pc->iomem, AM_PART_COMMAND, cmd);
}

static void partition_command_write(struct panthor_partition_control *pc,
				    u32 cmd, int aw_id)
{
	lockdep_assert_held(&pc->lock);

	partition_command_write_untracked(pc, cmd);

	pc->last_cmd.cmd = cmd;
	pc->last_cmd.aw_id = aw_id;
}

static int yield_now(struct panthor_partition_control *pc)
{
	guard(spinlock_irqsave)(&pc->lock);

	partition_command_write(pc, AM_PART_SET_COMMAND(PART_CMD_YIELD_NOW),
				pc->current_aw);

	return 0;
}

static int yield_idle(struct panthor_partition_control *pc)
{
	guard(spinlock_irqsave)(&pc->lock);

	/*
	 * This is a notification to yield the GPU once FW is idle. It is not
	 * a command that must succeed. Nothing of note is lost if the command
	 * fails apart from some minor scheduling optimisation losses. We can
	 * skip tracking this command.
	 */
	partition_command_write_untracked(
		pc, AM_PART_SET_COMMAND(PART_CMD_YIELD_IDLE));

	return 0;
}

static int window_close(struct panthor_partition_control *pc)
{
	/* Partition already closed. */
	if (partition_state_get(pc) == PART_STATE_RESET)
		return 1;

	guard(spinlock_irqsave)(&pc->lock);

	pc->closing = true;

	partition_command_write(pc, AM_PART_SET_COMMAND(PART_CMD_CLOSE_WINDOW),
				pc->current_aw);

	return 0;
}

static int window_open(struct panthor_partition_control *pc, u8 aw_id)
{
	if (aw_id >= AM_ARB_MAX_AW_COUNT)
		return -EINVAL;

	if (partition_state_get(pc) == PART_STATE_WINDOW_OPEN) {
		u8 cur_aw = partition_aw_get(pc);

		if (cur_aw == aw_id)
			return 0;

		dev_warn(pc->dev,
			 "%s: OPEN_WINDOW(%u): Already opened for aw=%u, state=%u. Closing...",
			 pc->name, aw_id, cur_aw, partition_state_get(pc));

		window_close(pc);
	}

	guard(spinlock_irqsave)(&pc->lock);

	partition_command_write(pc, (AM_PART_SET_COMMAND(PART_CMD_OPEN_WINDOW) |
				     AM_PART_SET_WINDOW(aw_id)), aw_id);

	pc->current_aw = aw_id;

	return 0;
}

static void partition_handle_reset_done(struct panthor_partition_control *pc)
{
	bool notify_stopped = false;
	int aw_id;

	gpu_write(pc->iomem, AM_PART_IRQ_CLEAR, PART_RESET_DONE);

	scoped_guard(spinlock_irqsave, &pc->lock) {
		aw_id = pc->current_aw;
		pc->current_aw = -1;

		/* RESET_DONE from CLOSE_WINDOW */
		if (pc->closing)
			pc->closing = false;
		if (aw_id >= 0)
			notify_stopped = true;
	}

	if (notify_stopped)
		panthor_arbitration_on_stopped(dev_get_drvdata(pc->dev), aw_id);
}

static void partition_handle_invalid_cmd(struct panthor_partition_control *pc)
{
	bool on_err = false;
	int aw_id;

	gpu_write(pc->iomem, AM_PART_IRQ_CLEAR, PART_INVALID_COMMAND);

	scoped_guard(spinlock_irqsave, &pc->lock) {
		if (!pc->last_cmd.cmd)
			return;

		if (pc->last_cmd.aw_id < 0)
			return;

		if (pc->last_cmd.aw_id != pc->current_aw)
			return;

		switch (AM_PART_GET_COMMAND(pc->last_cmd.cmd)) {
		case PART_CMD_YIELD_NOW:
		case PART_CMD_CLOSE_WINDOW:
			if (partition_state_get(pc) == PART_STATE_WINDOW_OPEN) {
				partition_command_write_untracked(pc, pc->last_cmd.cmd);
				return;
			}
			break;
		case PART_CMD_OPEN_WINDOW:
			if (partition_state_get(pc) == PART_STATE_RESET) {
				partition_command_write_untracked(pc, pc->last_cmd.cmd);
				return;
			} else if (partition_aw_get(pc) == pc->last_cmd.aw_id) {
				return;
			}

			on_err = true;
			aw_id = pc->last_cmd.aw_id;

			break;
		default:
			return;
		}
	}

	if (on_err)
		panthor_arbitration_notify_err(dev_get_drvdata(pc->dev), aw_id);
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
		partition_handle_invalid_cmd(pc);

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

		if (!WARN_ON(window_close(pc) < 0))
			WARN_ON(partition_state_wait(pc, PART_STATE_RESET));

		partition_irq_suspend(pc);
	}
}

static int partition_control_init(struct panthor_arbitration *adev,
				   const struct fwnode_handle *child)
{
	struct panthor_arbitration_resource res;
	struct panthor_partition_control *pc;
	struct device *dev = adev->dev;
	struct irq_data *irqdata;
	void __iomem *iomem;
	u32 irq_flags = 0;
	char *name;
	int irq;
	int ret;

	ret = panthor_arbitration_parse_resource(dev, child, &res);
	if (ret)
		return ret;

	if (res.id >= AM_ARB_MAX_PC_COUNT)
		return -ENODEV;

	if (adev->pc[res.id])
		return dev_warn_probe(dev, -ENODEV, "partition@%llx: id (%u) already present",
				      res.res.start, res.id);

	pc = devm_kzalloc(dev, sizeof(*pc), GFP_KERNEL);
	if (!pc)
		return -ENOMEM;

	name = devm_kasprintf(dev, GFP_KERNEL, "pc%d", res.id);

	iomem = devm_ioremap_resource(dev, &res.res);
	if (IS_ERR(iomem))
		return dev_err_probe(dev, PTR_ERR(iomem), "%s: Failed to ioremap", name);

	pc->name = name;
	pc->dev = dev;
	pc->iomem = iomem;

	spin_lock_init(&pc->lock);
	pc->current_aw = -1;

	irq = fwnode_irq_get(child, 0);
	if (irq < 0)
		return dev_err_probe(dev, irq, "%s: Failed to get IRQ", name);

	irqdata = irq_get_irq_data(irq);
	if (irqdata)
		irq_flags = irqd_get_trigger_type(irqdata);

	ret = devm_request_irq(
		dev, irq, partition_irq_raw_handler, irq_flags | IRQF_SHARED,
		devm_kasprintf(dev, GFP_KERNEL, "panthor-%s-irq", name), pc);
	if (ret)
		return ret;

	adev->pc[res.id] = pc;

	partition_irq_resume(pc);

	return 0;
}

int panthor_partition_control_init(struct panthor_arbitration *adev)
{
	device_for_each_named_child_node_scoped(adev->dev, child, "partition") {
		int ret = partition_control_init(adev, child);

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

int panthor_partition_control_yield_idle(struct panthor_partition_control *pc)
{
	return yield_idle(pc);
}
