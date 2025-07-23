// SPDX-License-Identifier: GPL-2.0
/* RPMsg Based Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/of.h>
#include "rpmsg_eth.h"

static int rpmsg_eth_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			      void *priv, u32 src)
{
	struct rpmsg_eth_common *common = dev_get_drvdata(&rpdev->dev);
	struct message *msg = (struct message *)data;
	u32 msg_type = msg->msg_hdr.msg_type;
	int ret = 0;

	switch (msg_type) {
	case RPMSG_ETH_REQUEST_MSG:
	case RPMSG_ETH_RESPONSE_MSG:
	case RPMSG_ETH_NOTIFY_MSG:
		dev_dbg(common->dev, "Msg type = %d, Src Id = %d\n",
			msg_type, msg->msg_hdr.src_id);
		break;
	default:
		dev_err(common->dev, "Invalid msg type\n");
		ret = -EINVAL;
		break;
	}
	return ret;
}

/**
 * rpmsg_eth_get_shm_info - Get shared memory info from device tree
 * @common: Pointer to rpmsg_eth_common structure
 *
 * Return: 0 on success, negative error code on failure
 */
static int rpmsg_eth_get_shm_info(struct rpmsg_eth_common *common)
{
	struct device_node *peer;
	const __be32 *reg;
	u64 start_address;
	int prop_size;
	int reg_len;
	u64 size;

	peer = of_find_node_by_name(NULL, "virtual-eth-shm");
	if (!peer) {
		dev_err(common->dev, "Couldn't get shared mem node");
		return -ENODEV;
	}

	reg = of_get_property(peer, "reg", &prop_size);
	if (!reg) {
		dev_err(common->dev, "Couldn't get reg property");
		return -ENODEV;
	}

	reg_len = prop_size / sizeof(u32);

	if (reg_len == 2) {
		/* 32-bit address space */
		start_address = be32_to_cpu(reg[0]);
		size = be32_to_cpu(reg[1]);
	} else if (reg_len == 4) {
		/* 64-bit address space */
		start_address = ((u64)be32_to_cpu(reg[0]) << 32) |
				 be32_to_cpu(reg[1]);
		size = ((u64)be32_to_cpu(reg[2]) << 32) |
			be32_to_cpu(reg[3]);
	} else {
		dev_err(common->dev, "Invalid reg_len: %d\n", reg_len);
		return -EINVAL;
	}

	common->port->buf_start_addr = start_address;
	common->port->buf_size = size;

	return 0;
}

static int rpmsg_eth_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct rpmsg_eth_common *common;
	int ret = 0;

	common = devm_kzalloc(&rpdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	dev_set_drvdata(dev, common);

	common->port = devm_kzalloc(dev, sizeof(*common->port), GFP_KERNEL);
	common->dev = dev;
	common->rpdev = rpdev;

	ret = rpmsg_eth_get_shm_info(common);
	if (ret)
		return ret;

	return 0;
}

static void rpmsg_eth_rpmsg_remove(struct rpmsg_device *rpdev)
{
	dev_dbg(&rpdev->dev, "rpmsg-eth client driver is removed\n");
}

static struct rpmsg_device_id rpmsg_eth_rpmsg_id_table[] = {
	{ .name = "shm-eth" },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_eth_rpmsg_id_table);

static struct rpmsg_driver rpmsg_eth_rpmsg_client = {
	.drv.name = KBUILD_MODNAME,
	.id_table = rpmsg_eth_rpmsg_id_table,
	.probe = rpmsg_eth_probe,
	.callback = rpmsg_eth_rpmsg_cb,
	.remove = rpmsg_eth_rpmsg_remove,
};
module_rpmsg_driver(rpmsg_eth_rpmsg_client);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MD Danish Anwar <danishanwar@ti.com>");
MODULE_DESCRIPTION("RPMsg Based Virtual Ethernet driver");
