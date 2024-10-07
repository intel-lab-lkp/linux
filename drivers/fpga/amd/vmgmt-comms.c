// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/timer.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#include "vmgmt.h"
#include "vmgmt-comms.h"

#define COMMS_PROTOCOL_VERSION			1
#define COMMS_PCI_BAR_OFF			0x2000000
#define COMMS_TIMER				(HZ / 10)
#define COMMS_DATA_LEN				16
#define COMMS_DATA_TYPE_MASK			GENMASK(7, 0)
#define COMMS_DATA_EOM_MASK			BIT(31)
#define COMMS_MSG_END				BIT(31)

#define COMMS_REG_WRDATA_OFF			0x0
#define COMMS_REG_RDDATA_OFF			0x8
#define COMMS_REG_STATUS_OFF			0x10
#define COMMS_REG_ERROR_OFF			0x14
#define COMMS_REG_RIT_OFF			0x1C
#define COMMS_REG_IS_OFF			0x20
#define COMMS_REG_IE_OFF			0x24
#define COMMS_REG_CTRL_OFF			0x2C
#define COMMS_REGS_SIZE				0x1000

#define COMMS_IRQ_DISABLE_ALL			0
#define COMMS_IRQ_RECEIVE_ENABLE		BIT(1)
#define COMMS_IRQ_CLEAR_ALL			GENMASK(2, 0)
#define COMMS_CLEAR_FIFO			GENMASK(1, 0)
#define COMMS_RECEIVE_THRESHOLD			15

enum comms_req_ops {
	COMMS_REQ_OPS_UNKNOWN			= 0,
	COMMS_REQ_OPS_HOT_RESET			= 5,
	COMMS_REQ_OPS_GET_PROTOCOL_VERSION	= 19,
	COMMS_REQ_OPS_GET_XCLBIN_UUID		= 20,
	COMMS_REQ_OPS_MAX,
};

enum comms_msg_type {
	COMMS_MSG_INVALID			= 0,
	COMMS_MSG_START				= 2,
	COMMS_MSG_BODY				= 3,
};

enum comms_msg_service_type {
	COMMS_MSG_SRV_RESPONSE			= BIT(0),
	COMMS_MSG_SRV_REQUEST			= BIT(1),
};

struct comms_hw_msg {
	struct {
		u32		type;
		u32		payload_size;
	} header;
	struct {
		u64		id;
		u32		flags;
		u32		size;
		u32		payload[COMMS_DATA_LEN - 6];
	} body;
} __packed;

struct comms_srv_req {
	u64			flags;
	u32			opcode;
	u32			data[];
};

struct comms_srv_ver_resp {
	u32			version;
};

struct comms_srv_uuid_resp {
	uuid_t			uuid;
};

struct comms_msg {
	u64			id;
	u32			flags;
	u32			len;
	u32			bytes_read;
	u32			data[10];
};

struct comms_device {
	struct vmgmt_device	*vdev;
	struct regmap		*regmap;
	struct timer_list	timer;
	struct work_struct	work;
};

static bool comms_regmap_rd_regs(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case COMMS_REG_RDDATA_OFF:
	case COMMS_REG_IS_OFF:
		return true;
	default:
		return false;
	}
}

static bool comms_regmap_wr_regs(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case COMMS_REG_WRDATA_OFF:
	case COMMS_REG_IS_OFF:
	case COMMS_REG_IE_OFF:
	case COMMS_REG_CTRL_OFF:
	case COMMS_REG_RIT_OFF:
		return true;
	default:
		return false;
	}
}

static bool comms_regmap_nir_regs(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case COMMS_REG_RDDATA_OFF:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config comms_regmap_config = {
	.name = "comms_config",
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.readable_reg = comms_regmap_rd_regs,
	.writeable_reg = comms_regmap_wr_regs,
	.readable_noinc_reg = comms_regmap_nir_regs,
};

static inline struct comms_device *to_ccdev_work(struct work_struct *w)
{
	return container_of(w, struct comms_device, work);
}

static inline struct comms_device *to_ccdev_timer(struct timer_list *t)
{
	return container_of(t, struct comms_device, timer);
}

static u32 comms_set_uuid_resp(struct vmgmt_device *vdev, void *payload)
{
	struct comms_srv_uuid_resp *resp;
	u32 resp_len = sizeof(*resp);

	resp = (struct comms_srv_uuid_resp *)payload;
	uuid_copy(&resp->uuid, &vdev->xclbin_uuid);
	vmgmt_dbg(vdev, "xclbin UUID: %pUb", &resp->uuid);

	return resp_len;
}

static u32 comms_set_protocol_resp(void *payload)
{
	struct comms_srv_ver_resp *resp = (struct comms_srv_ver_resp *)payload;
	u32 resp_len = sizeof(*resp);

	resp->version = COMMS_PROTOCOL_VERSION;

	return sizeof(resp_len);
}

static void comms_send_response(struct comms_device *ccdev,
				struct comms_msg *msg)
{
	struct comms_srv_req *req = (struct comms_srv_req *)msg->data;
	struct vmgmt_device *vdev = ccdev->vdev;
	struct comms_hw_msg response = {0};
	u32 size;
	int ret;
	u8 i;

	switch (req->opcode) {
	case COMMS_REQ_OPS_GET_PROTOCOL_VERSION:
		size = comms_set_protocol_resp(response.body.payload);
		break;
	case COMMS_REQ_OPS_GET_XCLBIN_UUID:
		size = comms_set_uuid_resp(vdev, response.body.payload);
		break;
	default:
		vmgmt_err(vdev, "Unsupported request opcode: %d", req->opcode);
		*response.body.payload = -1;
		size = sizeof(int);
	}

	vmgmt_dbg(vdev, "Response opcode: %d", req->opcode);

	response.header.type = COMMS_MSG_START | COMMS_MSG_END;
	response.header.payload_size = size;

	response.body.flags = COMMS_MSG_SRV_RESPONSE;
	response.body.size = size;
	response.body.id = msg->id;

	for (i = 0; i < COMMS_DATA_LEN; i++) {
		ret = regmap_write(ccdev->regmap, COMMS_REG_WRDATA_OFF, ((u32 *)&response)[i]);
		if (ret < 0) {
			vmgmt_err(vdev, "regmap write failed: %d", ret);
			return;
		}
	}
}

#define STATUS_IS_READY(status) ((status) & BIT(1))
#define STATUS_IS_ERROR(status) ((status) & BIT(2))

static void comms_check_request(struct work_struct *w)
{
	struct comms_device *ccdev = to_ccdev_work(w);
	u32 status = 0, request[COMMS_DATA_LEN] = {0};
	struct comms_hw_msg *hw_msg;
	struct comms_msg msg;
	u8 type, eom;
	int ret;
	int i;

	ret = regmap_read(ccdev->regmap, COMMS_REG_IS_OFF, &status);
	if (ret) {
		vmgmt_err(ccdev->vdev, "regmap read failed: %d", ret);
		return;
	}
	if (!STATUS_IS_READY(status))
		return;
	if (STATUS_IS_ERROR(status)) {
		vmgmt_err(ccdev->vdev, "An error has occurred with comms");
		return;
	}

	/* ACK status */
	regmap_write(ccdev->regmap, COMMS_REG_IS_OFF, status);

	for (i = 0; i < COMMS_DATA_LEN; i++) {
		if (regmap_read(ccdev->regmap, COMMS_REG_RDDATA_OFF, &request[i]) < 0) {
			vmgmt_err(ccdev->vdev, "regmap read failed");
			return;
		}
	}

	hw_msg = (struct comms_hw_msg *)request;
	type = FIELD_GET(COMMS_DATA_TYPE_MASK, hw_msg->header.type);
	eom = FIELD_GET(COMMS_DATA_EOM_MASK, hw_msg->header.type);

	/* Only support fixed size 64B messages */
	if (!eom || type != COMMS_MSG_START) {
		vmgmt_err(ccdev->vdev, "Unsupported message format or length");
		return;
	}

	msg.flags = hw_msg->body.flags;
	msg.len = hw_msg->body.size;
	msg.id = hw_msg->body.id;

	if (msg.flags != COMMS_MSG_SRV_REQUEST) {
		vmgmt_err(ccdev->vdev, "Unsupported service request");
		return;
	}

	if (hw_msg->body.size > sizeof(msg.data) * sizeof(msg.data[0])) {
		vmgmt_err(ccdev->vdev, "msg is too big: %d", hw_msg->body.size);
		return;
	}
	memcpy(msg.data, hw_msg->body.payload, hw_msg->body.size);

	/* Now decode and respond appropriately */
	comms_send_response(ccdev, &msg);
}

static void comms_sched_work(struct timer_list *t)
{
	struct comms_device *ccdev = to_ccdev_timer(t);

	/* Schedule a work in the general workqueue */
	schedule_work(&ccdev->work);
	/* Periodic timer */
	mod_timer(&ccdev->timer, jiffies + COMMS_TIMER);
}

static void comms_config(struct comms_device *ccdev)
{
	/* Disable interrupts */
	regmap_write(ccdev->regmap, COMMS_REG_IE_OFF, COMMS_IRQ_DISABLE_ALL);
	/* Clear request and response FIFOs */
	regmap_write(ccdev->regmap, COMMS_REG_CTRL_OFF, COMMS_CLEAR_FIFO);
	/* Clear interrupts */
	regmap_write(ccdev->regmap, COMMS_REG_IS_OFF, COMMS_IRQ_CLEAR_ALL);
	/* Setup RIT reg */
	regmap_write(ccdev->regmap, COMMS_REG_RIT_OFF, COMMS_RECEIVE_THRESHOLD);
	/* Enable RIT interrupt */
	regmap_write(ccdev->regmap, COMMS_REG_IE_OFF, COMMS_IRQ_RECEIVE_ENABLE);

	/* Create and schedule timer to do recurring work */
	INIT_WORK(&ccdev->work, &comms_check_request);
	timer_setup(&ccdev->timer, &comms_sched_work, 0);
	mod_timer(&ccdev->timer, jiffies + COMMS_TIMER);
}

void vmgmtm_comms_fini(struct comms_device *ccdev)
{
	/* First stop scheduling new work then cancel work */
	del_timer_sync(&ccdev->timer);
	cancel_work_sync(&ccdev->work);
}

struct comms_device *vmgmtm_comms_init(struct vmgmt_device *vdev)
{
	struct comms_device *ccdev;

	ccdev = devm_kzalloc(&vdev->pdev->dev, sizeof(*ccdev), GFP_KERNEL);
	if (!ccdev)
		return ERR_PTR(-ENOMEM);

	ccdev->vdev = vdev;

	ccdev->regmap = devm_regmap_init_mmio(&vdev->pdev->dev,
					      vdev->tbl + COMMS_PCI_BAR_OFF,
					      &comms_regmap_config);
	if (IS_ERR(ccdev->regmap)) {
		vmgmt_err(vdev, "Comms regmap init failed");
		return ERR_CAST(ccdev->regmap);
	}

	comms_config(ccdev);
	return ccdev;
}
