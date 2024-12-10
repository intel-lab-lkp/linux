// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/pci.h>

#include "versal-pci.h"
#include "versal-pci-comm-chan.h"

#define COMM_CHAN_PROTOCOL_VERSION		1
#define COMM_CHAN_PCI_BAR_OFF			0x2000000
#define COMM_CHAN_TIMER				(HZ / 10)
#define COMM_CHAN_DATA_LEN			16
#define COMM_CHAN_DATA_TYPE_MASK		GENMASK(7, 0)
#define COMM_CHAN_DATA_EOM_MASK			BIT(31)
#define COMM_CHAN_MSG_END			BIT(31)

#define COMM_CHAN_REG_WRDATA_OFF		0x0
#define COMM_CHAN_REG_RDDATA_OFF		0x8
#define COMM_CHAN_REG_STATUS_OFF		0x10
#define COMM_CHAN_REG_ERROR_OFF			0x14
#define COMM_CHAN_REG_RIT_OFF			0x1C
#define COMM_CHAN_REG_IS_OFF			0x20
#define COMM_CHAN_REG_IE_OFF			0x24
#define COMM_CHAN_REG_CTRL_OFF			0x2C
#define COMM_CHAN_REGS_SIZE			SZ_4K

#define COMM_CHAN_IRQ_DISABLE_ALL		0
#define COMM_CHAN_IRQ_RECEIVE_ENABLE		BIT(1)
#define COMM_CHAN_IRQ_CLEAR_ALL			GENMASK(2, 0)
#define COMM_CHAN_CLEAR_FIFO			GENMASK(1, 0)
#define COMM_CHAN_RECEIVE_THRESHOLD		15

enum comm_chan_req_ops {
	COMM_CHAN_REQ_OPS_UNKNOWN		= 0,
	COMM_CHAN_REQ_OPS_HOT_RESET		= 5,
	COMM_CHAN_REQ_OPS_GET_PROTOCOL_VERSION	= 19,
	COMM_CHAN_REQ_OPS_LOAD_XCLBIN_UUID	= 20,
	COMM_CHAN_REQ_OPS_MAX,
};

enum comm_chan_msg_type {
	COMM_CHAN_MSG_INVALID			= 0,
	COMM_CHAN_MSG_START			= 2,
	COMM_CHAN_MSG_BODY			= 3,
};

enum comm_chan_msg_service_type {
	COMM_CHAN_MSG_SRV_RESPONSE		= BIT(0),
	COMM_CHAN_MSG_SRV_REQUEST		= BIT(1),
};

struct comm_chan_hw_msg {
	struct {
		__u32		type;
		__u32		payload_size;
	} header;
	struct {
		__u64		id;
		__u32		flags;
		__u32		size;
		__u32		payload[COMM_CHAN_DATA_LEN - 6];
	} body;
} __packed;

struct comm_chan_srv_req {
	__u64			flags;
	__u32			opcode;
	__u32			data[];
};

struct comm_chan_srv_ver_resp {
	__u32			version;
};

struct comm_chan_srv_uuid_resp {
	__u32			ret;
};

struct comm_chan_msg {
	__u64			id;
	__u32			flags;
	__u32			len;
	__u32			bytes_read;
	__u32			data[10];
};

struct comm_chan_device {
	struct versal_pci_device	*vdev;
	struct timer_list		timer;
	struct work_struct		work;
};

static inline struct comm_chan_device *to_ccdev_work(struct work_struct *w)
{
	return container_of(w, struct comm_chan_device, work);
}

static inline struct comm_chan_device *to_ccdev_timer(struct timer_list *t)
{
	return container_of(t, struct comm_chan_device, timer);
}

static inline u32 comm_chan_read(struct comm_chan_device *cdev, u32 offset)
{
	return readl(cdev->vdev->io_regs + COMM_CHAN_PCI_BAR_OFF + offset);
}

static inline void comm_chan_write(struct comm_chan_device *cdev, u32 offset, const u32 value)
{
	writel(value, cdev->vdev->io_regs + COMM_CHAN_PCI_BAR_OFF + offset);
}

static u32 comm_chan_set_uuid_resp(void *payload, int ret)
{
	struct comm_chan_srv_uuid_resp *resp = (struct comm_chan_srv_uuid_resp *)payload;
	u32 resp_len = sizeof(*resp);

	resp->ret = (u32)ret;

	return resp_len;
}

static u32 comm_chan_set_protocol_resp(void *payload)
{
	struct comm_chan_srv_ver_resp *resp = (struct comm_chan_srv_ver_resp *)payload;
	u32 resp_len = sizeof(*resp);

	resp->version = COMM_CHAN_PROTOCOL_VERSION;

	return sizeof(resp_len);
}

static void comm_chan_send_response(struct comm_chan_device *ccdev, u64 msg_id, void *payload)
{
	struct comm_chan_srv_req *req = (struct comm_chan_srv_req *)payload;
	struct versal_pci_device *vdev = ccdev->vdev;
	struct comm_chan_hw_msg response = {0};
	u32 size;
	int ret;
	u8 i;

	switch (req->opcode) {
	case COMM_CHAN_REQ_OPS_GET_PROTOCOL_VERSION:
		size = comm_chan_set_protocol_resp(response.body.payload);
		break;
	case COMM_CHAN_REQ_OPS_LOAD_XCLBIN_UUID:
		ret = versal_pci_load_xclbin(vdev, (uuid_t *)req->data);
		size = comm_chan_set_uuid_resp(response.body.payload, ret);
		break;
	default:
		vdev_err(vdev, "Unsupported request opcode: %d", req->opcode);
		*response.body.payload = -1;
		size = sizeof(int);
	}

	vdev_dbg(vdev, "Response opcode: %d", req->opcode);

	response.header.type = COMM_CHAN_MSG_START | COMM_CHAN_MSG_END;
	response.header.payload_size = size;

	response.body.flags = COMM_CHAN_MSG_SRV_RESPONSE;
	response.body.size = size;
	response.body.id = msg_id;

	for (i = 0; i < COMM_CHAN_DATA_LEN; i++)
		comm_chan_write(ccdev, COMM_CHAN_REG_WRDATA_OFF, ((u32 *)&response)[i]);
}

#define STATUS_IS_READY(status) ((status) & BIT(1))
#define STATUS_IS_ERROR(status) ((status) & BIT(2))

static void comm_chan_check_request(struct work_struct *w)
{
	struct comm_chan_device *ccdev = to_ccdev_work(w);
	u32 status = 0, request[COMM_CHAN_DATA_LEN] = {0};
	struct comm_chan_hw_msg *hw_msg;
	u8 type, eom;
	int i;

	status = comm_chan_read(ccdev, COMM_CHAN_REG_IS_OFF);
	if (!STATUS_IS_READY(status))
		return;
	if (STATUS_IS_ERROR(status)) {
		vdev_err(ccdev->vdev, "An error has occurred with comms");
		return;
	}

	/* ACK status */
	comm_chan_write(ccdev, COMM_CHAN_REG_IS_OFF, status);

	for (i = 0; i < COMM_CHAN_DATA_LEN; i++)
		request[i] = comm_chan_read(ccdev, COMM_CHAN_REG_RDDATA_OFF);

	hw_msg = (struct comm_chan_hw_msg *)request;
	type = FIELD_GET(COMM_CHAN_DATA_TYPE_MASK, hw_msg->header.type);
	eom = FIELD_GET(COMM_CHAN_DATA_EOM_MASK, hw_msg->header.type);

	/* Only support fixed size 64B messages */
	if (!eom || type != COMM_CHAN_MSG_START) {
		vdev_err(ccdev->vdev, "Unsupported message format or length");
		return;
	}

	if (hw_msg->body.flags != COMM_CHAN_MSG_SRV_REQUEST) {
		vdev_err(ccdev->vdev, "Unsupported service request");
		return;
	}

	if (hw_msg->body.size > sizeof(hw_msg->body.payload)) {
		vdev_err(ccdev->vdev, "msg is too big: %d", hw_msg->body.size);
		return;
	}

	/* Now decode and respond appropriately */
	comm_chan_send_response(ccdev, hw_msg->body.id, hw_msg->body.payload);
}

static void comm_chan_sched_work(struct timer_list *t)
{
	struct comm_chan_device *ccdev = to_ccdev_timer(t);

	/* Schedule a work in the general workqueue */
	schedule_work(&ccdev->work);
	/* Periodic timer */
	mod_timer(&ccdev->timer, jiffies + COMM_CHAN_TIMER);
}

static void comm_chan_config(struct comm_chan_device *ccdev)
{
	/* Disable interrupts */
	comm_chan_write(ccdev, COMM_CHAN_REG_IE_OFF, COMM_CHAN_IRQ_DISABLE_ALL);
	/* Clear request and response FIFOs */
	comm_chan_write(ccdev, COMM_CHAN_REG_CTRL_OFF, COMM_CHAN_CLEAR_FIFO);
	/* Clear interrupts */
	comm_chan_write(ccdev, COMM_CHAN_REG_IS_OFF, COMM_CHAN_IRQ_CLEAR_ALL);
	/* Setup RIT reg */
	comm_chan_write(ccdev, COMM_CHAN_REG_RIT_OFF, COMM_CHAN_RECEIVE_THRESHOLD);
	/* Enable RIT interrupt */
	comm_chan_write(ccdev, COMM_CHAN_REG_IE_OFF, COMM_CHAN_IRQ_RECEIVE_ENABLE);

	/* Create and schedule timer to do recurring work */
	INIT_WORK(&ccdev->work, &comm_chan_check_request);
	timer_setup(&ccdev->timer, &comm_chan_sched_work, 0);
	mod_timer(&ccdev->timer, jiffies + COMM_CHAN_TIMER);
}

void versal_pci_comm_chan_fini(struct comm_chan_device *ccdev)
{
	/* First stop scheduling new work then cancel work */
	del_timer_sync(&ccdev->timer);
	cancel_work_sync(&ccdev->work);
}

struct comm_chan_device *versal_pci_comm_chan_init(struct versal_pci_device *vdev)
{
	struct comm_chan_device *ccdev;

	ccdev = devm_kzalloc(&vdev->pdev->dev, sizeof(*ccdev), GFP_KERNEL);
	if (!ccdev)
		return ERR_PTR(-ENOMEM);

	ccdev->vdev = vdev;

	comm_chan_config(ccdev);
	return ccdev;
}
