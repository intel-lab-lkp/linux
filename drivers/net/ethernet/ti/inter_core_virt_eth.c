// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#include "inter_core_virt_eth.h"

static int icve_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			 void *priv, u32 src)
{
	struct icve_common *common = dev_get_drvdata(&rpdev->dev);
	struct message *msg = (struct message *)data;
	u32 msg_type = msg->msg_hdr.msg_type;
	u32 rpmsg_type;

	switch (msg_type) {
	case ICVE_REQUEST_MSG:
		rpmsg_type = msg->req_msg.type;
		dev_dbg(common->dev, "Msg type = %d; RPMsg type = %d\n",
			msg_type, rpmsg_type);
		break;
	case ICVE_RESPONSE_MSG:
		rpmsg_type = msg->resp_msg.type;
		dev_dbg(common->dev, "Msg type = %d; RPMsg type = %d\n",
			msg_type, rpmsg_type);
		break;
	case ICVE_NOTIFY_MSG:
		rpmsg_type = msg->notify_msg.type;
		dev_dbg(common->dev, "Msg type = %d; RPMsg type = %d\n",
			msg_type, rpmsg_type);
		break;
	default:
		dev_err(common->dev, "Invalid msg type\n");
		break;
	}
	return 0;
}

static int icve_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct icve_common *common;

	common = devm_kzalloc(&rpdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	dev_set_drvdata(dev, common);

	common->port = devm_kzalloc(dev, sizeof(*common->port), GFP_KERNEL);
	common->dev = dev;
	common->rpdev = rpdev;

	return 0;
}

static void icve_rpmsg_remove(struct rpmsg_device *rpdev)
{
	dev_info(&rpdev->dev, "icve rpmsg client driver is removed\n");
}

static struct rpmsg_device_id icve_rpmsg_id_table[] = {
	{ .name = "ti.icve" },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, icve_rpmsg_id_table);

static struct rpmsg_driver icve_rpmsg_client = {
	.drv.name = KBUILD_MODNAME,
	.id_table = icve_rpmsg_id_table,
	.probe = icve_rpmsg_probe,
	.callback = icve_rpmsg_cb,
	.remove = icve_rpmsg_remove,
};
module_rpmsg_driver(icve_rpmsg_client);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Siddharth Vadapalli <s-vadapalli@ti.com>");
MODULE_AUTHOR("Ravi Gunasekaran <r-gunasekaran@ti.com");
MODULE_DESCRIPTION("TI Inter Core Virtual Ethernet driver");
