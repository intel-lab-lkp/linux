// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/pci.h>

#include "versal-pci.h"
#include "versal-pci-rm-queue.h"
#include "versal-pci-rm-service.h"

static inline struct rm_device *to_rdev_msg_monitor(struct work_struct *w)
{
	return container_of(w, struct rm_device, msg_monitor);
}

static inline struct rm_device *to_rdev_msg_timer(struct timer_list *t)
{
	return container_of(t, struct rm_device, msg_timer);
}

static inline u32 rm_io_read(struct rm_device *rdev, u32 offset)
{
	return rm_reg_read(rdev, RM_PCI_IO_BAR_OFF + offset);
}

static inline int rm_io_write(struct rm_device *rdev, u32 offset, u32 value)
{
	rm_reg_write(rdev, RM_PCI_IO_BAR_OFF + offset, value);
	return 0;
}

static inline u32 rm_queue_read(struct rm_device *rdev, u32 offset)
{
	return rm_reg_read(rdev, RM_PCI_SHMEM_BAR_OFF + rdev->queue_base + offset);
}

static inline void rm_queue_write(struct rm_device *rdev, u32 offset, u32 value)
{
	rm_reg_write(rdev, RM_PCI_SHMEM_BAR_OFF + rdev->queue_base + offset, value);
}

static inline void rm_queue_bulk_read(struct rm_device *rdev, u32 offset,
				      u32 *value, u32 size)
{
	rm_bulk_reg_read(rdev, RM_PCI_SHMEM_BAR_OFF + rdev->queue_base + offset, value, size);
}

static inline void rm_queue_bulk_write(struct rm_device *rdev, u32 offset,
				       u32 *value, u32 size)
{
	rm_bulk_reg_write(rdev, RM_PCI_SHMEM_BAR_OFF + rdev->queue_base + offset, value, size);
}

static inline u32 rm_queue_get_cidx(struct rm_device *rdev, enum rm_queue_type type)
{
	u32 off;

	if (type == RM_QUEUE_SQ)
		off = offsetof(struct rm_queue_header, sq_cidx);
	else
		off = offsetof(struct rm_queue_header, cq_cidx);

	return rm_queue_read(rdev, off);
}

static inline void rm_queue_set_cidx(struct rm_device *rdev, enum rm_queue_type type,
				     u32 value)
{
	u32 off;

	if (type == RM_QUEUE_SQ)
		off = offsetof(struct rm_queue_header, sq_cidx);
	else
		off = offsetof(struct rm_queue_header, cq_cidx);

	rm_queue_write(rdev, off, value);
}

static inline u32 rm_queue_get_pidx(struct rm_device *rdev, enum rm_queue_type type)
{
	if (type == RM_QUEUE_SQ)
		return rm_io_read(rdev, RM_IO_SQ_PIDX_OFF);
	else
		return rm_io_read(rdev, RM_IO_CQ_PIDX_OFF);
}

static inline int rm_queue_set_pidx(struct rm_device *rdev,
				    enum rm_queue_type type, u32 value)
{
	if (type == RM_QUEUE_SQ)
		return rm_io_write(rdev, RM_IO_SQ_PIDX_OFF, value);
	else
		return rm_io_write(rdev, RM_IO_CQ_PIDX_OFF, value);
}

static inline u32 rm_queue_get_sq_slot_offset(struct rm_device *rdev)
{
	u32 index;

	if ((rdev->sq.pidx - rdev->sq.cidx) >= rdev->queue_size)
		return RM_INVALID_SLOT;

	index = rdev->sq.pidx & (rdev->queue_size - 1);
	return rdev->sq.offset + RM_CMD_SQ_SLOT_SIZE * index;
}

static inline u32 rm_queue_get_cq_slot_offset(struct rm_device *rdev)
{
	u32 index;

	index = rdev->cq.cidx & (rdev->queue_size - 1);
	return rdev->cq.offset + RM_CMD_CQ_SLOT_SIZE * index;
}

static int rm_queue_submit_cmd(struct rm_cmd *cmd)
{
	struct versal_pci_device *vdev = cmd->rdev->vdev;
	struct rm_device *rdev = cmd->rdev;
	u32 offset;
	int ret;

	mutex_lock(&rdev->queue);

	offset = rm_queue_get_sq_slot_offset(rdev);
	if (!offset) {
		vdev_err(vdev, "No SQ slot available");
		ret = -ENOSPC;
		goto exit;
	}

	rm_queue_bulk_write(rdev, offset, (u32 *)&cmd->sq_msg,
			    sizeof(cmd->sq_msg));

	ret = rm_queue_set_pidx(rdev, RM_QUEUE_SQ, ++rdev->sq.pidx);
	if (ret) {
		vdev_err(vdev, "Failed to update PIDX, ret %d", ret);
		goto exit;
	}

	list_add_tail(&cmd->list, &rdev->submitted_cmds);
exit:
	mutex_unlock(&rdev->queue);
	return ret;
}

void rm_queue_withdraw_cmd(struct rm_cmd *cmd)
{
	mutex_lock(&cmd->rdev->queue);
	list_del(&cmd->list);
	mutex_unlock(&cmd->rdev->queue);
}

static int rm_queue_wait_cmd_timeout(struct rm_cmd *cmd, unsigned long timeout)
{
	struct versal_pci_device *vdev = cmd->rdev->vdev;
	int ret;

	if (wait_for_completion_timeout(&cmd->executed, timeout)) {
		ret = cmd->cq_msg.data.rcode;
		if (!ret)
			return 0;

		vdev_err(vdev, "CMD returned with a failure: %d", ret);
		return ret;
	}

	/*
	 * each cmds will be cleaned up by complete before it times out.
	 * if we reach here, the cmd should be cleared and hot reset should
	 * be issued.
	 */
	vdev_err(vdev, "cmd is timedout after, please reset the card");
	rm_queue_withdraw_cmd(cmd);
	return -ETIME;
}

int rm_queue_send_cmd(struct rm_cmd *cmd, unsigned long timeout)
{
	int ret;

	ret = rm_queue_submit_cmd(cmd);
	if (ret)
		return ret;

	return rm_queue_wait_cmd_timeout(cmd, timeout);
}

static int rm_process_msg(struct rm_device *rdev)
{
	struct versal_pci_device *vdev = rdev->vdev;
	struct rm_cmd *cmd, *next;
	struct rm_cmd_cq_hdr header;
	u32 offset;

	offset = rm_queue_get_cq_slot_offset(rdev);
	if (!offset) {
		vdev_err(vdev, "Invalid CQ offset");
		return -EINVAL;
	}

	rm_queue_bulk_read(rdev, offset, (u32 *)&header, sizeof(header));

	list_for_each_entry_safe(cmd, next, &rdev->submitted_cmds, list) {
		u32 value = 0;

		if (cmd->sq_msg.hdr.id != header.id)
			continue;

		rm_queue_bulk_read(rdev, offset + sizeof(cmd->cq_msg.hdr),
				   (u32 *)&cmd->cq_msg.data,
				   sizeof(cmd->cq_msg.data));

		rm_queue_write(rdev, offset, value);

		list_del(&cmd->list);
		complete(&cmd->executed);
		return 0;
	}

	vdev_err(vdev, "Unknown cmd ID %d found in CQ", header.id);
	return -EFAULT;
}

static void rm_check_msg(struct work_struct *w)
{
	struct rm_device *rdev = to_rdev_msg_monitor(w);
	int ret;

	mutex_lock(&rdev->queue);

	rdev->sq.cidx = rm_queue_get_cidx(rdev, RM_QUEUE_SQ);
	rdev->cq.pidx = rm_queue_get_pidx(rdev, RM_QUEUE_CQ);

	while (rdev->cq.cidx < rdev->cq.pidx) {
		ret = rm_process_msg(rdev);
		if (ret)
			break;

		rdev->cq.cidx++;

		rm_queue_set_cidx(rdev, RM_QUEUE_CQ, rdev->cq.cidx);
	}

	mutex_unlock(&rdev->queue);
}

static void rm_sched_work(struct timer_list *t)
{
	struct rm_device *rdev = to_rdev_msg_timer(t);

	/* Schedule a work in the general workqueue */
	schedule_work(&rdev->msg_monitor);
	/* Periodic timer */
	mod_timer(&rdev->msg_timer, jiffies + RM_COMPLETION_TIMER);
}

void rm_queue_fini(struct rm_device *rdev)
{
	del_timer_sync(&rdev->msg_timer);
	cancel_work_sync(&rdev->msg_monitor);
	mutex_destroy(&rdev->queue);
}

int rm_queue_init(struct rm_device *rdev)
{
	struct versal_pci_device *vdev = rdev->vdev;
	struct rm_queue_header header = {0};
	int ret;

	INIT_LIST_HEAD(&rdev->submitted_cmds);
	mutex_init(&rdev->queue);

	rm_queue_bulk_read(rdev, RM_HDR_OFF, (u32 *)&header, sizeof(header));

	if (header.magic != RM_QUEUE_HDR_MAGIC_NUM) {
		vdev_err(vdev, "Invalid RM queue header");
		ret = -ENODEV;
		goto error;
	}

	if (!header.version) {
		vdev_err(vdev, "Invalid RM queue header");
		ret = -ENODEV;
		goto error;
	}

	sema_init(&rdev->sq.data_lock, 1);
	sema_init(&rdev->cq.data_lock, 1);
	rdev->queue_size = header.size;
	rdev->sq.offset = header.sq_off;
	rdev->cq.offset = header.cq_off;
	rdev->sq.type = RM_QUEUE_SQ;
	rdev->cq.type = RM_QUEUE_CQ;
	rdev->sq.data_size = rdev->queue_buffer_size - RM_CMD_CQ_BUFFER_SIZE;
	rdev->cq.data_size = RM_CMD_CQ_BUFFER_SIZE;
	rdev->sq.data_offset = rdev->queue_buffer_start +
			       RM_CMD_CQ_BUFFER_OFFSET + RM_CMD_CQ_BUFFER_SIZE;
	rdev->cq.data_offset = rdev->queue_buffer_start +
			       RM_CMD_CQ_BUFFER_OFFSET;
	rdev->sq.cidx = header.sq_cidx;
	rdev->cq.cidx = header.cq_cidx;

	rdev->sq.pidx = rm_queue_get_pidx(rdev, RM_QUEUE_SQ);
	rdev->cq.pidx = rm_queue_get_pidx(rdev, RM_QUEUE_CQ);

	if (rdev->cq.cidx != rdev->cq.pidx) {
		vdev_warn(vdev, "Clearing stale completions");
		rdev->cq.cidx = rdev->cq.pidx;
		rm_queue_set_cidx(rdev, RM_QUEUE_CQ, rdev->cq.cidx);
	}

	/* Create and schedule timer to do recurring work */
	INIT_WORK(&rdev->msg_monitor, &rm_check_msg);
	timer_setup(&rdev->msg_timer, &rm_sched_work, 0);
	mod_timer(&rdev->msg_timer, jiffies + RM_COMPLETION_TIMER);

	return 0;
error:
	mutex_destroy(&rdev->queue);
	return ret;
}
