/* SPDX-License-Identifier: GPL-2.0 */
/* Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#include "inter-core-virt-eth.h"

static int icve_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	struct icve_common *common = dev_get_drvdata(&rpdev->dev);
	struct message *msg = (struct message *)data;
	struct icve_port *port = common->port;
	u32 msg_type = msg->msg_hdr.msg_type;
	u32 rpmsg_type;

	switch (msg_type) {
	case ICVE_RESPONSE_MSG:
		rpmsg_type = msg->resp_msg.type;
		switch (rpmsg_type) {
		case ICVE_RESP_SHM_INFO:

			/* Retrieve Tx and Rx shared memory info from msg */
			port->tx_buffer = msg->resp_msg.shm_info.tx_buffer;

			if (!port->tx_buffer) {
				dev_err(common->dev, "Tx Buffer invalid\n");
				return -ENOMEM;
			}

			port->tx_buffer->base_addr =
				msg->resp_msg.shm_info.tx_buffer_base_addr;

			if (!port->tx_buffer->base_addr) {
				dev_err(common->dev, "Tx Buffer address invalid\n");
				return -ENOMEM;
			}

			port->rx_buffer = msg->resp_msg.shm_info.rx_buffer;

			if (!port->rx_buffer) {
				dev_err(common->dev, "Rx Buffer invalid\n");
				return -ENOMEM;
			}

			port->rx_buffer->base_addr =
				msg->resp_msg.shm_info.rx_buffer_base_addr;

			if (!port->rx_buffer->base_addr) {
				dev_err(common->dev, "Rx Buffer address invalid\n");
				return -ENOMEM;
			}

			port->icve_max_buffers =
				msg->resp_msg.shm_info.max_buffers;

			break;
		}
		break;
	default:
		dev_err(common->dev, "Invalid msg type\n");
		break;
	}

	return 0;
}

static int create_request(struct icve_common *common, enum icve_rpmsg_type rpmsg_type)
{
	struct message *msg = &common->send_msg;
	int ret = 0;

	msg->msg_hdr.src_id = common->port->port_id;
	msg->req_msg.type = rpmsg_type;

	switch (rpmsg_type) {
	case ICVE_REQ_SHM_INFO:
		msg->msg_hdr.msg_type = ICVE_REQUEST_MSG;
		break;
	default:
		ret = -EINVAL;
		dev_err(common->dev, "Invalid RPMSG request\n");
	};

	return ret;
}

static int icve_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct icve_common *common;
	unsigned long flags;

	common = devm_kzalloc(&rpdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	dev_set_drvdata(dev, common);

	common->port = devm_kzalloc(dev, sizeof(*common->port), GFP_KERNEL);
	common->dev = dev;
	common->rpdev = rpdev;

	spin_lock_init(&common->send_msg_lock);
	spin_lock_init(&common->recv_msg_lock);

	/* Send request to fetch shared memory details from remote core */
	spin_lock_irqsave(&common->send_msg_lock, flags);
	create_request(common, ICVE_REQ_SHM_INFO);
	rpmsg_send(common->rpdev->ept, (void *)(&common->send_msg), sizeof(common->send_msg));
	spin_unlock_irqrestore(&common->send_msg_lock, flags);

	return 0;
}

static void icve_rpmsg_remove(struct rpmsg_device *rpdev)
{
	dev_info(&rpdev->dev, "icve rpmsg client driver is removed\n");
}

static struct rpmsg_device_id icve_rpmsg_id_table[] = {
	{ .name = "icve-rpsmg-client" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, icve_rpmsg_id_table);

static struct rpmsg_driver icve_rpmsg_client = {
	.drv.name	= KBUILD_MODNAME,
	.id_table	= icve_rpmsg_id_table,
	.probe		= icve_rpmsg_probe,
	.callback	= icve_rpmsg_cb,
	.remove		= icve_rpmsg_remove,
};
module_rpmsg_driver(icve_rpmsg_client);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Siddharth Vadapalli <s-vadapalli@ti.com>");
MODULE_AUTHOR("Ravi Gunasekaran <r-gunasekaran@ti.com");
MODULE_DESCRIPTION("TI Inter Core Virtual Ethernet driver");
