// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2026 ARM Limited. All rights reserved. */

#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "panthor_am_msg.h"
#include "panthor_arbitration.h"
#include "panthor_device_io.h"
#include "panthor_resource_group.h"

#define AM_RG_IRQ_RAWSTAT			0x80
#define AM_RG_IRQ_CLEAR				0x84
#define AM_RG_IRQ_MASK				0x88
#define AM_RG_IRQ_STATUS			0x8C
#define   AM_RG_MESSAGE_MASK			GENMASK(15, 0)

#define AM_RG_AW_MESSAGE(n)			(0x100 + (n) * 0x20)

struct rg_aw_data {
	/** @msg: Messaging data */
	struct panthor_am_msg msg;
};

struct panthor_resource_group {
	/** @dev: Device pointer */
	struct device *dev;

	/** @name: Resource name */
	char *name;

	/** @iomem: CPU mapping of the partition control IOMEM region */
	void __iomem *iomem;

	/** @irq: IRQ number */
	int irq;

	/** @aw_data: Data for each access window */
	struct rg_aw_data aw_data[AM_ARB_MAX_AW_COUNT];

	/** @wq: Resource group workqueue */
	struct workqueue_struct *wq;

	/** @msg_retry_work: Work to retry sending pending messages */
	struct work_struct msg_retry_work;
};

static void rg_retry_messages(struct work_struct *work)
{
	struct panthor_resource_group *rg = container_of(
		work, struct panthor_resource_group, msg_retry_work);
	bool retry = false;

	for (u8 aw_id = 0; aw_id < AM_ARB_MAX_AW_COUNT; aw_id++) {
		struct panthor_am_msg *msg = &rg->aw_data[aw_id].msg;
		int ret;

		ret = panthor_am_msg_retry(msg);
		if (ret == -EBUSY || ret == -EAGAIN)
			retry = true;

		if (ret == -EINVAL)
			dev_warn(rg->dev, "AW%u send FIFO unexpectedy empty", aw_id);
	}

	if (retry)
		queue_work(rg->wq, &rg->msg_retry_work);
}

static void rg_send_msg(struct panthor_resource_group *rg, u8 aw_id,
			u64 message)
{
	int ret;

	ret = panthor_am_msg_send(&rg->aw_data[aw_id].msg, message);
	if (ret == -ENOMEM)
		dev_err(rg->dev, "AW%u send FIFO is full", aw_id);

	if (ret == -EBUSY) {
		dev_dbg(rg->dev, "AW%u has pending messages, scheduling retry work", aw_id);
		queue_work(rg->wq, &rg->msg_retry_work);
	}
}

static void rg_respond_to_handshake(struct panthor_resource_group *rg,
				    u8 aw_id, u64 message)
{
	struct rg_aw_data *aw_data = &rg->aw_data[aw_id];
	bool acked = AM_MSG_ACK_GET(message);
	u8 version = AM_MSG_VERSION_GET(message);
	int ret;

	ret = panthor_am_msg_version_validate(&aw_data->msg, version);
	if (ret == -EOPNOTSUPP)
		dev_warn(rg->dev,
			 "AW%u protocol version less than minimum supported (%u < %u)",
			 aw_id, version, AM_MSG_MIN_SUPPORTED_VERSION);

	if (!acked) {
		/* AW initiated the handshake, reply with supported version */
		u64 reply = ARB_VM_INIT_MAKE(1, aw_data->msg.version);

		rg_send_msg(rg, aw_id, reply);
	}
}

static void rg_handshake_init(struct panthor_resource_group *rg)
{
	for (u8 aw_id = 0; aw_id < AM_ARB_MAX_AW_COUNT; aw_id++) {
		u64 message = ARB_VM_INIT_MAKE(0, AM_MSG_CURRENT_VERSION);

		rg_send_msg(rg, aw_id, message);
	}
}

static void rg_handle_message(struct panthor_resource_group *rg, u8 aw_id,
			      u64 message)
{
	u8 msg_id = AM_MSG_ID_GET(message);
	if (!msg_id)
		return;

	switch (msg_id) {
	case VM_ARB_INIT:
		rg_respond_to_handshake(rg, aw_id, message);
		break;
	case VM_ARB_GPU_REQUEST:
		/* TODO: on_request */
		break;
	case VM_ARB_GPU_STOPPED:
		/* TODO: on_idle */
		break;
	default:
		dev_warn(rg->dev, "Invalid message (0x%llx)", message);
		break;
	}
}

static irqreturn_t rg_irq_raw_handler(int irq, void *data)
{
	struct panthor_resource_group *rg = data;
	unsigned long message_mask;
	u32 status;
	u8 aw_id;

	status = gpu_read(rg->iomem, AM_RG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	message_mask = status;
	for_each_set_bit(aw_id, &message_mask, AM_ARB_MAX_AW_COUNT) {
		u64 message = panthor_am_msg_read(&rg->aw_data[aw_id].msg);

		rg_handle_message(rg, aw_id, message);
	}

	gpu_write(rg->iomem, AM_RG_IRQ_CLEAR, status);

	return IRQ_HANDLED;
}

static void rg_irq_suspend(struct panthor_resource_group *rg)
{
	gpu_write(rg->iomem, AM_RG_IRQ_MASK, 0);
}

static void rg_irq_resume(struct panthor_resource_group *rg)
{
	gpu_write(rg->iomem, AM_RG_IRQ_MASK, AM_RG_MESSAGE_MASK);
}

int panthor_resource_group_suspend(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_RG_COUNT; i++) {
		struct panthor_resource_group *rg = adev->rg[i];

		if (!rg)
			continue;

		rg_irq_suspend(rg);
	}

	return 0;
}

int panthor_resource_group_resume(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_RG_COUNT; i++) {
		struct panthor_resource_group *rg = adev->rg[i];

		if (!rg)
			continue;

		rg_irq_resume(rg);
	}

	return 0;
}

void panthor_resource_group_term(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_RG_COUNT; i++) {
		struct panthor_resource_group *rg = adev->rg[i];

		if (!rg)
			continue;

		disable_work_sync(&rg->msg_retry_work);
		rg_irq_suspend(rg);
	}
}

static int resource_group_init(struct panthor_arbitration *adev, int i)
{
	struct device *dev = adev->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct panthor_resource_group *rg;
	struct resource *res;
	void __iomem *iomem;
	char *name;
	int ret = 0;
	int irq;

	name = devm_kasprintf(dev, GFP_KERNEL, "rg%d", i);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res) {
		devm_kfree(dev, name);
		return -ENODEV;
	}

	iomem = devm_ioremap_resource(dev, res);
	if (IS_ERR(iomem)) {
		dev_err(dev, "%s: Failed to ioremap", name);
		return PTR_ERR(iomem);
	}

	rg = devm_kzalloc(dev, sizeof(*rg), GFP_KERNEL);
	if (!rg)
		return -ENOMEM;

	rg->dev = dev;
	rg->iomem = iomem;

	rg->wq = devm_alloc_ordered_workqueue(dev, "panthor-%s-wq", 0, name);
	if (!rg->wq)
		return -ENOMEM;

	INIT_WORK(&rg->msg_retry_work, rg_retry_messages);

	for (int i = 0; i < AM_ARB_MAX_AW_COUNT; i++)
		panthor_am_msg_init(&rg->aw_data[i].msg,
				    rg->iomem + AM_RG_AW_MESSAGE(i));

	adev->rg[i] = rg;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(
		dev, irq, rg_irq_raw_handler, IRQF_SHARED,
		devm_kasprintf(dev, GFP_KERNEL, "panthor-%s-irq", name), rg);
	if (ret)
		return ret;

	rg_irq_resume(rg);

	rg_handshake_init(rg);

	return 0;
}

int panthor_resource_group_init(struct panthor_arbitration *adev)
{
	for (int i = 0; i < AM_ARB_MAX_RG_COUNT; i++) {
		int ret = resource_group_init(adev, i);
		if (ret == -ENODEV)
			continue;

		if (ret)
			return ret;
	}

	return 0;
}
