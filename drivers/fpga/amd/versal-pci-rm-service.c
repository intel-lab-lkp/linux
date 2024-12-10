// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/vmalloc.h>

#include "versal-pci.h"
#include "versal-pci-rm-service.h"
#include "versal-pci-rm-queue.h"

static DEFINE_IDA(rm_cmd_ids);

static void rm_uninstall_health_monitor(struct rm_device *rdev);

static inline struct rm_device *to_rdev_health_monitor(struct work_struct *w)
{
	return container_of(w, struct rm_device, health_monitor);
}

static inline struct rm_device *to_rdev_health_timer(struct timer_list *t)
{
	return container_of(t, struct rm_device, health_timer);
}

u32 rm_reg_read(struct rm_device *rdev, u32 offset)
{
	return readl(rdev->vdev->io_regs + offset);
}

void rm_reg_write(struct rm_device *rdev, u32 offset, const u32 value)
{
	writel(value, rdev->vdev->io_regs + offset);
}

void rm_bulk_reg_read(struct rm_device *rdev, u32 offset, u32 *value, size_t size)
{
	void __iomem *src = rdev->vdev->io_regs + offset;
	void *dst = (void *)value;

	memcpy_fromio(dst, src, size);
	/* Barrier of reading data from device */
	rmb();
}

void rm_bulk_reg_write(struct rm_device *rdev, u32 offset, const void *value, size_t size)
{
	void __iomem *dst = rdev->vdev->io_regs + offset;

	memcpy_toio(dst, value, size);
	/* Barrier of writing data to device */
	wmb();
}

static inline u32 rm_shmem_read(struct rm_device *rdev, u32 offset)
{
	return rm_reg_read(rdev, RM_PCI_SHMEM_BAR_OFF + offset);
}

static inline void rm_shmem_bulk_read(struct rm_device *rdev, u32 offset,
				      u32 *value, u32 size)
{
	rm_bulk_reg_read(rdev, RM_PCI_SHMEM_BAR_OFF + offset, value, size);
}

static inline void rm_shmem_bulk_write(struct rm_device *rdev, u32 offset,
				       const void *value, u32 size)
{
	rm_bulk_reg_write(rdev, RM_PCI_SHMEM_BAR_OFF + offset, value, size);
}

void rm_queue_destory_cmd(struct rm_cmd *cmd)
{
	ida_free(&rm_cmd_ids, cmd->sq_msg.hdr.id);
	kfree(cmd);
}

static int rm_queue_copy_response(struct rm_cmd *cmd, void *buffer, ssize_t len)
{
	struct rm_cmd_cq_log_page *result = &cmd->cq_msg.data.page;
	u64 off = cmd->sq_msg.data.page.address;

	if (!result->len || len < result->len) {
		vdev_err(cmd->rdev->vdev, "Invalid response or buffer size");
		return -EINVAL;
	}

	rm_shmem_bulk_read(cmd->rdev, off, (u32 *)buffer, result->len);
	return 0;
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
		vdev_err(rdev->vdev, "Unsupported file size");
		return -ENOMEM;
	}

	ret = down_interruptible(&rdev->sq.data_lock);
	if (ret)
		return ret;

	rm_shmem_bulk_write(cmd->rdev, rdev->sq.data_offset, buffer, size);

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
		vdev_err(rdev->vdev, "Invalid cmd opcode %d", opcode);
		ret = -EINVAL;
		goto error;
	}

	cmd->opcode = opcode;
	cmd->sq_msg.hdr.opcode = FIELD_PREP(RM_CMD_SQ_HDR_OPS_MSK, opcode);
	cmd->sq_msg.hdr.msg_size = FIELD_PREP(RM_CMD_SQ_HDR_SIZE_MSK, size);

	id = ida_alloc_range(&rm_cmd_ids, RM_CMD_ID_MIN, RM_CMD_ID_MAX, GFP_KERNEL);
	if (id < 0) {
		vdev_err(rdev->vdev, "Failed to alloc cmd ID: %d", id);
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
	struct versal_pci_device *vdev = rdev->vdev;
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
	vdev_dbg(vdev, "VMR version %d.%d", major, minor);
	if (!major) {
		vdev_err(vdev, "VMR version is unsupported");
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

int rm_queue_boot_apu(struct rm_device *rdev)
{
	char *bin = "xilinx/xrt-versal-apu.xsabin";
	const struct firmware *fw = NULL;
	bool status;
	int ret;

	ret = rm_check_apu_status(rdev, &status);
	if (ret) {
		vdev_err(rdev->vdev, "Failed to get APU status");
		return ret;
	}

	if (status) {
		vdev_dbg(rdev->vdev, "APU online. Skipping APU FW download");
		return 0;
	}

	ret = request_firmware(&fw, bin, &rdev->vdev->pdev->dev);
	if (ret) {
		vdev_warn(rdev->vdev, "Request APU FW %s failed %d", bin, ret);
		return ret;
	}

	vdev_dbg(rdev->vdev, "Starting... APU FW download");
	ret = rm_download_apu_fw(rdev, (char *)fw->data, fw->size);
	vdev_dbg(rdev->vdev, "Finished... APU FW download %d", ret);

	if (ret)
		vdev_err(rdev->vdev, "Failed to download APU FW, ret:%d", ret);

	release_firmware(fw);

	return ret;
}

static void rm_check_health(struct work_struct *w)
{
	struct rm_device *rdev = to_rdev_health_monitor(w);
	u32 max_len = PAGE_SIZE;
	struct rm_cmd *cmd;
	int ret;

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_GET_LOG_PAGE, &cmd);
	if (ret)
		return;

	ret = rm_queue_payload_init(cmd, RM_CMD_LOG_PAGE_AXI_TRIP_STATUS);
	if (ret)
		goto destroy_cmd;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_CONFIG_TIMEOUT);
	if (ret == -ETIME || ret == -EINVAL)
		goto payload_fini;

	if (ret) {
		u32 log_len = cmd->cq_msg.data.page.len;

		if (log_len > max_len) {
			vdev_warn(rdev->vdev, "msg size %d is greater than requested %d",
				  log_len, max_len);
			log_len = max_len;
		}

		if (log_len) {
			char *buffer = vzalloc(log_len);

			if (!buffer)
				goto payload_fini;

			ret = rm_queue_copy_response(cmd, buffer, log_len);
			if (ret) {
				vfree(buffer);
				goto payload_fini;
			}

			vdev_err(rdev->vdev, "%s", buffer);
			vfree(buffer);

		} else {
			vdev_err(rdev->vdev, "firewall check ret%d", ret);
		}

		rdev->firewall_tripped = 1;
	}

payload_fini:
	rm_queue_payload_fini(cmd);
destroy_cmd:
	rm_queue_destory_cmd(cmd);

	vdev_dbg(rdev->vdev, "check result: %d", ret);
}

static void rm_sched_health_check(struct timer_list *t)
{
	struct rm_device *rdev = to_rdev_health_timer(t);

	if (rdev->firewall_tripped) {
		vdev_err(rdev->vdev, "Firewall tripped, health check paused. Please reset card");
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

void versal_pci_rm_fini(struct rm_device *rdev)
{
	rm_uninstall_health_monitor(rdev);
	rm_queue_fini(rdev);
}

struct rm_device *versal_pci_rm_init(struct versal_pci_device *vdev)
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

	rm_shmem_bulk_read(rdev, RM_HDR_OFF, (u32 *)header, sizeof(*header));
	if (header->magic != RM_HDR_MAGIC_NUM) {
		vdev_err(vdev, "Invalid RM header 0x%x", header->magic);
		ret = -ENODEV;
		goto err;
	}

	status = rm_shmem_read(rdev, header->status_off);
	if (!status) {
		vdev_err(vdev, "RM status %d is not ready", status);
		ret = -ENODEV;
		goto err;
	}

	rdev->queue_buffer_size = header->data_end - header->data_start + 1;
	rdev->queue_buffer_start = header->data_start;
	rdev->queue_base = header->queue_base;

	ret = rm_queue_init(rdev);
	if (ret) {
		vdev_err(vdev, "Failed to init cmd queue, ret %d", ret);
		ret = -ENODEV;
		goto err;
	}

	ret = rm_queue_verify(rdev);
	if (ret) {
		vdev_err(vdev, "Failed to verify cmd queue, ret %d", ret);
		ret = -ENODEV;
		goto queue_fini;
	}

	ret = rm_queue_boot_apu(rdev);
	if (ret) {
		vdev_err(vdev, "Failed to bringup APU, ret %d", ret);
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

int rm_queue_get_fw_id(struct rm_device *rdev)
{
	struct rm_cmd *cmd;
	int ret;

	ret = rm_queue_create_cmd(rdev, RM_QUEUE_OP_GET_LOG_PAGE, &cmd);
	if (ret)
		return ret;

	ret = rm_queue_payload_init(cmd, RM_CMD_LOG_PAGE_FW_ID);
	if (ret)
		goto destroy_cmd;

	ret = rm_queue_send_cmd(cmd, RM_CMD_WAIT_CONFIG_TIMEOUT);
	if (ret)
		goto payload_fini;

	ret = rm_queue_copy_response(cmd, rdev->vdev->fw_id, sizeof(rdev->vdev->fw_id));
	if (ret)
		goto payload_fini;

	vdev_info(rdev->vdev, "fw_id %s", rdev->vdev->fw_id);

payload_fini:
	rm_queue_payload_fini(cmd);
destroy_cmd:
	rm_queue_destory_cmd(cmd);

	return ret;
}
