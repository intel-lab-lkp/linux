// SPDX-License-Identifier: GPL-2.0
/* RPMsg Based Virtual Ethernet Driver
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
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
 * rpmsg_eth_get_shm_info - Retrieve shared memory region for RPMsg Ethernet
 * @common: Pointer to rpmsg_eth_common structure
 *
 * This function locates and maps the reserved memory region for the RPMsg
 * Ethernet device by traversing the device tree hierarchy. It first identifies
 * the associated remote processor (rproc), then locates the "rpmsg-eth" child
 * node within the rproc's device tree node, and finally retrieves the
 * "memory-region" phandle that points to the reserved memory region.
 * Once found, the shared memory region is mapped into the
 * kernel's virtual address space using devm_ioremap()
 *
 * Return: 0 on success, negative error code on failure.
 */
static int rpmsg_eth_get_shm_info(struct rpmsg_eth_common *common)
{
	struct device_node *np, *rpmsg_eth_node, *rmem_np;
	struct reserved_mem *rmem;
	struct rproc *rproc;

	/* Get the remote processor associated with this device */
	rproc = rproc_get_by_child(&common->rpdev->dev);
	if (!rproc) {
		dev_err(common->dev, "rpmsg eth device not child of rproc\n");
		return -EINVAL;
	}

	/* Get the device node from rproc or its parent */
	np = rproc->dev.of_node ?: (rproc->dev.parent ? rproc->dev.parent->of_node : NULL);
	if (!np) {
		dev_err(common->dev, "Cannot find rproc device node\n");
		return -ENODEV;
	}

	/* Get the rpmsg-eth child node */
	rpmsg_eth_node = of_get_child_by_name(np, "rpmsg-eth");
	if (!rpmsg_eth_node) {
		dev_err(common->dev, "Couldn't get rpmsg-eth node from np\n");
		return -ENODEV;
	}

	/* Parse the memory-region phandle */
	rmem_np = of_parse_phandle(rpmsg_eth_node, "memory-region", 0);
	of_node_put(rpmsg_eth_node);
	if (!rmem_np)
		return -EINVAL;

	/* Lookup the reserved memory region */
	rmem = of_reserved_mem_lookup(rmem_np);
	of_node_put(rmem_np);
	if (!rmem)
		return -EINVAL;

	common->port->shm = devm_ioremap(common->dev, rmem->base, rmem->size);
	if (IS_ERR(common->port->shm))
		return PTR_ERR(common->port->shm);

	common->port->buf_size = rmem->size;

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

static void rpmsg_eth_remove(struct rpmsg_device *rpdev)
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
	.remove = rpmsg_eth_remove,
};
module_rpmsg_driver(rpmsg_eth_rpmsg_client);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MD Danish Anwar <danishanwar@ti.com>");
MODULE_DESCRIPTION("RPMsg Based Virtual Ethernet driver");
