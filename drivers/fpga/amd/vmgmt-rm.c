// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#include "vmgmt.h"
#include "vmgmt-rm.h"
#include "vmgmt-rm-queue.h"

static DEFINE_IDA(rm_cmd_ids);

static const struct regmap_config rm_shmem_regmap_config = {
	.name = "rm_shmem_config",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
};

static const struct regmap_config rm_io_regmap_config = {
	.name = "rm_io_config",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
};

static void rm_uninstall_health_monitor(struct rm_device *rdev);

static inline struct rm_device *to_rdev_health_monitor(struct work_struct *w)
{
	return container_of(w, struct rm_device, health_monitor);
}

static inline struct rm_device *to_rdev_health_timer(struct timer_list *t)
{
	return container_of(t, struct rm_device, health_timer);
}

static inline int rm_shmem_read(struct rm_device *rdev, u32 offset, u32 *value)
{
	return regmap_read(rdev->shmem_regmap, offset, value);
}

static inline int rm_shmem_bulk_read(struct rm_device *rdev, u32 offset,
				     u32 *value, u32 size)
{
	return regmap_bulk_read(rdev->shmem_regmap, offset, value,
				DIV_ROUND_UP(size, 4));
}

static inline int rm_shmem_bulk_write(struct rm_device *rdev, u32 offset,
				      u32 *value, u32 size)
{
	return regmap_bulk_write(rdev->shmem_regmap, offset, value,
				DIV_ROUND_UP(size, 4));
}

void rm_queue_destory_cmd(struct rm_cmd *cmd)
{
	ida_free(&rm_cmd_ids, cmd->sq_msg.hdr.id);
	kfree(cmd);
}

int rm_queue_copy_response(struct rm_cmd *cmd, void *buffer, ssize_t len)
{
	struct rm_cmd_cq_log_page *result = &cmd->cq_msg.data.page;
	u64 off = cmd->sq_msg.data.page.address;

	if (!result->len || len < result->len) {
		vmgmt_err(cmd->rdev->vdev, "Invalid response or buffer size");
		return -EINVAL;
	}

	return rm_shmem_bulk_read(cmd->rdev, off, (u32 *)buffer, result->len);
}

static void rm_queue_payload_fini(struct rm_cmd *cmd)
{
	up(&cmd->rdev->cq.data_lock);
}

static int rm_queue_payload_init(struct rm_cmd *cmd,
				 enum rm_cmd_log_page_type type)
{
	struct rm_device *rdev = cmd->rdev;
	int ret;

	ret = down_interruptible(&rdev->cq.data_lock);
	if (ret)
		return ret;

	cmd->sq_msg.data.page.address = rdev->cq.data_offset;
	cmd->sq_msg.data.page.size = rdev->cq.data_size;
	cmd->sq_msg.data.page.reserved1 = 0;
	cmd->sq_msg.data.page.type = FIELD_PREP(RM_CMD_LOG_PAGE_TYPE_MASK,
						type);
	return 0;
}

void rm_queue_data_fini(struct rm_cmd *cmd)
{
	up(&cmd->rdev->sq.data_lock);
}

int rm_queue_data_init(struct rm_cmd *cmd, const char *buffer, ssize_t size)
{
	struct rm_device *rdev = cmd->rdev;
	int ret;

	if (!size || size > rdev->sq.data_size) {
		vmgmt_err(rdev->vdev, "Unsupported file size");
		return -ENOMEM;
	}

	ret = down_interruptible(&rdev->sq.data_lock);
	if (ret)
		return ret;

	ret = rm_shmem_bulk_write(cmd->rdev, rdev->sq.data_offset,
				  (u32 *)buffer, size);
	if (ret) {
		vmgmt_err(rdev->vdev, "Failed to copy binary to SQ buffer");
		up(&cmd->rdev->sq.data_lock);
		return ret;
	}

	cmd->sq_msg.data.bin.address = rdev->sq.data_offset;
	cmd->sq_msg.data.bin.size = size;
	return 0;
}

int rm_queue_create_cmd(struct rm_device *rdev, enum rm_queue_opcode opcode,
			struct rm_cmd **cmd_ptr)
{
	struct rm_cmd *cmd = NULL;
	int ret, id;
	u16 size;

	if (rdev->firewall_tripped)
		return -ENODEV;

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	cmd->rdev = rdev;

	switch (opcode) {
	case RM_QUEUE_OP_LOAD_XCLBIN:
		fallthrough;
	case RM_QUEUE_OP_LOAD_FW:
		fallthrough;
	case RM_QUEUE_OP_LOAD_APU_FW:
		size = sizeof(struct rm_cmd_sq_bin);
		break;
	case RM_QUEUE_OP_GET_LOG_PAGE:
		size = sizeof(struct rm_cmd_sq_log_page);
		break;
	case RM_QUEUE_OP_IDENTIFY:
		size = 0;
		break;
	case RM_QUEUE_OP_VMR_CONTROL:
		size = sizeof(struct rm_cmd_sq_ctrl);
		break;
	default:
		vmgmt_err(rdev->vdev, "Invalid cmd opcode %d", opcode);
		ret = -EINVAL;
		goto error;
	};

	cmd->opcode = opcode;
	cmd->sq_msg.hdr.opcode = FIELD_PREP(RM_CMD_SQ_HDR_OPS_MSK, opcode);
	cmd->sq_msg.hdr.msg_size = FIELD_PREP(RM_CMD_SQ_HDR_SIZE_MSK, size);

	id = ida_alloc_range(&rm_cmd_ids, RM_CMD_ID_MIN, RM_CMD_ID_MAX, GFP_KERNEL);
	if (id < 0) {
		vmgmt_err(rdev->vdev, "Failed to alloc cmd ID: %d", id);
		ret = id;
		goto error;
	}
	cmd->sq_msg.hdr.id = id;

	init_completion(&cmd->executed);

	*cmd_ptr = cmd;
	return 0;
error:
	kfree(cmd);
	return ret;
}

static int rm_queue_verify(struct rm_device *rdev)
{
	struct vmgmt_device *vdev = rdev->vdev;
	struct rm_cmd_cq_identify *result;
	struct rm_cmd *cmd;
	u32 major, minor;
	int ret;

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_IDENTIFY, &cmd);
	if (ret)
		return ret;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_CONFIG_TIMEOUT);
	if (ret)
		goto error;

	result = &cmd->cq_msg.data.identify;
	major = result->major;
	minor = result->minor;
	vmgmt_dbg(vdev, "VMR version %d.%d", major, minor);
	if (!major) {
		vmgmt_err(vdev, "VMR version is unsupported");
		ret = -EOPNOTSUPP;
	}

error:
	rm_queue_destory_cmd(cmd);
	return ret;
}

static int rm_check_apu_status(struct rm_device *rdev, bool *status)
{
	struct rm_cmd_cq_control *result;
	struct rm_cmd *cmd;
	int ret;

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_VMR_CONTROL, &cmd);
	if (ret)
		return ret;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_CONFIG_TIMEOUT);
	if (ret)
		goto error;

	result = &cmd->cq_msg.data.ctrl;
	*status = FIELD_GET(RM_CMD_VMR_CONTROL_PS_MASK, result->status);

	rm_queue_destory_cmd(cmd);
	return 0;

error:
	rm_queue_destory_cmd(cmd);
	return ret;
}

static int rm_download_apu_fw(struct rm_device *rdev, char *data, ssize_t size)
{
	struct rm_cmd *cmd;
	int ret;

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_LOAD_APU_FW, &cmd);
	if (ret)
		return ret;

	ret = rm_queue_data_init(cmd, data, size);
	if (ret)
		goto done;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_DOWNLOAD_TIMEOUT);

done:
	rm_queue_destory_cmd(cmd);
	return ret;
}

int rm_boot_apu(struct rm_device *rdev)
{
	char *bin = "xilinx/xrt-versal-apu.xsabin";
	const struct firmware *fw = NULL;
	bool status;
	int ret;

	ret = rm_check_apu_status(rdev, &status);
	if (ret) {
		vmgmt_err(rdev->vdev, "Failed to get APU status");
		return ret;
	}

	if (status) {
		vmgmt_dbg(rdev->vdev, "APU online. Skipping APU FW download");
		return 0;
	}

	ret = request_firmware(&fw, bin, &rdev->vdev->pdev->dev);
	if (ret) {
		vmgmt_warn(rdev->vdev, "Request APU FW %s failed %d", bin, ret);
		return ret;
	}

	vmgmt_dbg(rdev->vdev, "Starting... APU FW download");
	ret = rm_download_apu_fw(rdev, (char *)fw->data, fw->size);
	vmgmt_dbg(rdev->vdev, "Finished... APU FW download %d", ret);

	if (ret)
		vmgmt_err(rdev->vdev, "Failed to download APU FW, ret:%d", ret);

	release_firmware(fw);

	return ret;
}

static void rm_check_health(struct work_struct *w)
{
	struct rm_device *rdev = to_rdev_health_monitor(w);
	ssize_t len = PAGE_SIZE;
	char *buffer = NULL;
	struct rm_cmd *cmd;
	int ret;

	buffer = vzalloc(len);
	if (!buffer)
		return;

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_GET_LOG_PAGE, &cmd);
	if (ret)
		return;

	ret = rm_queue_payload_init(cmd, RM_CMD_LOG_PAGE_AXI_TRIP_STATUS);
	if (ret)
		goto error;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_CONFIG_TIMEOUT);
	if (ret == -ETIME || ret == -EINVAL)
		goto payload_fini;

	if (cmd->cq_msg.data.page.len) {
		ret = rm_queue_copy_response(cmd, buffer, len);
		if (ret)
			goto payload_fini;

		vmgmt_err(rdev->vdev, "%s", buffer);
		rdev->firewall_tripped = 1;
	}

	vfree(buffer);

	rm_queue_payload_fini(cmd);
	rm_queue_destory_cmd(cmd);

	return;

payload_fini:
	rm_queue_payload_fini(cmd);
error:
	rm_queue_destory_cmd(cmd);
	vfree(buffer);
}

static void rm_sched_health_check(struct timer_list *t)
{
	struct rm_device *rdev = to_rdev_health_timer(t);

	if (rdev->firewall_tripped) {
		vmgmt_err(rdev->vdev, "Firewall tripped, health check paused. Please reset card");
		return;
	}
	/* Schedule a work in the general workqueue */
	schedule_work(&rdev->health_monitor);
	/* Periodic timer */
	mod_timer(&rdev->health_timer, jiffies + RM_HEALTH_CHECK_TIMER);
}

static void rm_uninstall_health_monitor(struct rm_device *rdev)
{
	del_timer_sync(&rdev->health_timer);
	cancel_work_sync(&rdev->health_monitor);
}

static void rm_install_health_monitor(struct rm_device *rdev)
{
	INIT_WORK(&rdev->health_monitor, &rm_check_health);
	timer_setup(&rdev->health_timer, &rm_sched_health_check, 0);
	mod_timer(&rdev->health_timer, jiffies + RM_HEALTH_CHECK_TIMER);
}

void vmgmt_rm_fini(struct rm_device *rdev)
{
	rm_uninstall_health_monitor(rdev);
	rm_queue_fini(rdev);
}

struct rm_device *vmgmt_rm_init(struct vmgmt_device *vdev)
{
	struct rm_header *header;
	struct rm_device *rdev;
	u32 status;
	int ret;

	rdev = devm_kzalloc(&vdev->pdev->dev, sizeof(*rdev), GFP_KERNEL);
	if (!rdev)
		return ERR_PTR(-ENOMEM);

	rdev->vdev = vdev;
	header = &rdev->rm_metadata;

	rdev->shmem_regmap = devm_regmap_init_mmio(&vdev->pdev->dev,
						   vdev->tbl + RM_PCI_SHMEM_BAR_OFF,
						   &rm_shmem_regmap_config);
	if (IS_ERR(rdev->shmem_regmap)) {
		vmgmt_err(vdev, "Failed to init RM shared memory regmap");
		return ERR_CAST(rdev->shmem_regmap);
	}

	ret = rm_shmem_bulk_read(rdev, RM_HDR_OFF, (u32 *)header,
				 sizeof(*header));
	if (ret) {
		vmgmt_err(vdev, "Failed to read RM shared mem, ret %d", ret);
		ret = -ENODEV;
		goto err;
	}

	if (header->magic != RM_HDR_MAGIC_NUM) {
		vmgmt_err(vdev, "Invalid RM header 0x%x", header->magic);
		ret = -ENODEV;
		goto err;
	}

	ret = rm_shmem_read(rdev, header->status_off, &status);
	if (ret) {
		vmgmt_err(vdev, "Failed to read RM shared mem, ret %d", ret);
		ret = -ENODEV;
		goto err;
	}

	if (!status) {
		vmgmt_err(vdev, "RM status %d is not ready", status);
		ret = -ENODEV;
		goto err;
	}

	rdev->queue_buffer_size = header->data_end - header->data_start + 1;
	rdev->queue_buffer_start = header->data_start;
	rdev->queue_base = header->queue_base;

	rdev->io_regmap = devm_regmap_init_mmio(&vdev->pdev->dev,
						vdev->tbl + RM_PCI_IO_BAR_OFF,
						&rm_io_regmap_config);
	if (IS_ERR(rdev->io_regmap)) {
		vmgmt_err(vdev, "Failed to init RM IO regmap");
		ret = PTR_ERR(rdev->io_regmap);
		goto err;
	}

	ret = rm_queue_init(rdev);
	if (ret) {
		vmgmt_err(vdev, "Failed to init cmd queue, ret %d", ret);
		ret = -ENODEV;
		goto err;
	}

	ret = rm_queue_verify(rdev);
	if (ret) {
		vmgmt_err(vdev, "Failed to verify cmd queue, ret %d", ret);
		ret = -ENODEV;
		goto queue_fini;
	}

	ret = rm_boot_apu(rdev);
	if (ret) {
		vmgmt_err(vdev, "Failed to bringup APU, ret %d", ret);
		ret = -ENODEV;
		goto queue_fini;
	}

	rm_install_health_monitor(rdev);

	return rdev;
queue_fini:
	rm_queue_fini(rdev);
err:
	return ERR_PTR(ret);
}

int vmgmt_rm_get_fw_id(struct rm_device *rdev, uuid_t *uuid)
{
	char str[UUID_STRING_LEN];
	ssize_t len = PAGE_SIZE;
	char *buffer = NULL;
	struct rm_cmd *cmd;
	u8 i, j;
	int ret;

	buffer = vmalloc(len);
	if (!buffer)
		return -ENOMEM;

	memset(buffer, 0, len);

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_GET_LOG_PAGE, &cmd);
	if (ret)
		return ret;

	ret = rm_queue_payload_init(cmd, RM_CMD_LOG_PAGE_FW_ID);
	if (ret)
		goto error;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_CONFIG_TIMEOUT);
	if (ret)
		goto payload;

	ret = rm_queue_copy_response(cmd, buffer, len);
	if (ret)
		goto payload;

	/* parse uuid into a valid uuid string format */
	for (i  = 0, j = 0; i < strlen(buffer); i++) {
		str[j++] = buffer[i];
		if (j == 8 || j == 13 || j == 18 || j == 23)
			str[j++] = '-';
	}

	uuid_parse(str, uuid);
	vmgmt_dbg(rdev->vdev, "Interface uuid %pU", uuid);

	vfree(buffer);

	rm_queue_payload_fini(cmd);
	rm_queue_destory_cmd(cmd);

	return 0;

payload:
	rm_queue_payload_fini(cmd);
error:
	rm_queue_destory_cmd(cmd);
	vfree(buffer);
	return ret;
}
