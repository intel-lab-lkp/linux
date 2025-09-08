// SPDX-License-Identifier: GPL-2.0
/* RPMsg Based Virtual Ethernet Driver
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include "rpmsg_eth.h"

/**
 * rpmsg_eth_validate_handshake - Validate handshake parameters from remote
 * @port: Pointer to rpmsg_eth_port structure
 * @shm_info: Pointer to shared memory info received from remote
 *
 * Checks buffer size, magic numbers, and TX/RX offsets in the handshake
 * response to ensure they match expected values and are within valid ranges.
 *
 * Return: 0 on success, -EINVAL on validation failure.
 */
static int rpmsg_eth_validate_handshake(struct rpmsg_eth_port *port,
					struct rpmsg_eth_shm *shm_info)
{
	if (shm_info->buff_slot_size != RPMSG_ETH_BUFFER_SIZE) {
		dev_err(port->common->dev, "Buffer configuration mismatch in handshake: expected_buf_size=%zu, received_buf_size=%d\n",
			RPMSG_ETH_BUFFER_SIZE,
			shm_info->buff_slot_size);
		return -EINVAL;
	}

	if (readl(port->shm + port->tx_offset + HEAD_MAGIC_NUM_OFFSET) != RPMSG_ETH_SHM_MAGIC_NUM ||
	    readl(port->shm + port->rx_offset + HEAD_MAGIC_NUM_OFFSET) != RPMSG_ETH_SHM_MAGIC_NUM ||
	    readl(port->shm + port->tx_offset + TAIL_MAGIC_NUM_OFFSET(port->tx_max_buffers)) != RPMSG_ETH_SHM_MAGIC_NUM ||
	    readl(port->shm + port->rx_offset + TAIL_MAGIC_NUM_OFFSET(port->rx_max_buffers)) != RPMSG_ETH_SHM_MAGIC_NUM) {
		dev_err(port->common->dev, "Magic number mismatch in handshake at head/tail\n");
		return -EINVAL;
	}

	if (shm_info->tx_offset >= port->buf_size ||
	    shm_info->rx_offset >= port->buf_size) {
		dev_err(port->common->dev, "TX/RX offset out of range in handshake: tx_offset=0x%x, rx_offset=0x%x, size=0x%llx\n",
			shm_info->tx_offset,
			shm_info->rx_offset,
			port->buf_size);
		return -EINVAL;
	}

	return 0;
}

static int rpmsg_eth_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			      void *priv, u32 src)
{
	struct rpmsg_eth_common *common = dev_get_drvdata(&rpdev->dev);
	struct message *msg = (struct message *)data;
	struct rpmsg_eth_port *port = common->port;
	u32 msg_type = msg->msg_hdr.msg_type;
	u32 rpmsg_type;
	int ret = 0;

	switch (msg_type) {
	case RPMSG_ETH_REQUEST_MSG:
		rpmsg_type = msg->req_msg.type;
		dev_dbg(common->dev, "Msg type = %d, RPMsg type = %d, Src Id = %d, Msg Id = %d\n",
			msg_type, rpmsg_type, msg->msg_hdr.src_id, msg->req_msg.id);
		break;
	case RPMSG_ETH_RESPONSE_MSG:
		rpmsg_type = msg->resp_msg.type;
		dev_dbg(common->dev, "Msg type = %d, RPMsg type = %d, Src Id = %d, Msg Id = %d\n",
			msg_type, rpmsg_type, msg->msg_hdr.src_id, msg->resp_msg.id);
		switch (rpmsg_type) {
		case RPMSG_ETH_RESP_SHM_INFO:
			/* Retrieve Tx and Rx shared memory info from msg */
			port->tx_offset = msg->resp_msg.shm_info.tx_offset;
			port->rx_offset = msg->resp_msg.shm_info.rx_offset;
			port->tx_max_buffers =
				msg->resp_msg.shm_info.num_pkt_bufs;
			port->rx_max_buffers =
				msg->resp_msg.shm_info.num_pkt_bufs;

			/* Handshake validation */
			ret = rpmsg_eth_validate_handshake(port, &msg->resp_msg.shm_info);
			if (ret) {
				dev_err(common->dev, "RPMSG handshake failed %d\n", ret);
				return ret;
			}
			break;
		}
		break;
	case RPMSG_ETH_NOTIFY_MSG:
		rpmsg_type = msg->notify_msg.type;
		dev_dbg(common->dev, "Msg type = %d, RPMsg type = %d, Src Id = %d, Msg Id = %d\n",
			msg_type, rpmsg_type, msg->msg_hdr.src_id, msg->notify_msg.id);
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
	struct device_node *np, *rmem_np;
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

	/* Parse the memory-region phandle */
	rmem_np = of_parse_phandle(np, "memory-region", common->data.shm_region_index);
	of_node_put(np);
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

static int rpmsg_eth_init_ndev(struct rpmsg_eth_common *common)
{
	struct device *dev = &common->rpdev->dev;
	struct rpmsg_eth_ndev_priv *ndev_priv;
	struct rpmsg_eth_port *port;
	static u32 port_id;
	int err = 0;

	port = common->port;
	port->common = common;
	port->port_id = port_id++;

	port->ndev = devm_alloc_etherdev_mqs(common->dev, sizeof(*ndev_priv),
					     RPMSG_ETH_MAX_TX_QUEUES,
					     RPMSG_ETH_MAX_RX_QUEUES);

	if (!port->ndev) {
		dev_err(dev, "error allocating net_device\n");
		return -ENOMEM;
	}

	ndev_priv = netdev_priv(port->ndev);
	ndev_priv->port = port;
	SET_NETDEV_DEV(port->ndev, dev);

	port->ndev->min_mtu = RPMSG_ETH_MIN_PACKET_SIZE;
	port->ndev->max_mtu = MAX_MTU;

	if (!is_valid_ether_addr(port->ndev->dev_addr)) {
		eth_hw_addr_random(port->ndev);
		dev_dbg(dev, "Using random MAC address %pM\n", port->ndev->dev_addr);
	}

	netif_carrier_off(port->ndev);
	err = register_netdev(port->ndev);
	if (err)
		dev_err(dev, "error registering rpmsg_eth net device %d\n", err);

	return err;
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
	common->data = *(const struct rpmsg_eth_data *)rpdev->id.driver_data;
	dev_err(dev, "shm_index = %d\n", common->data.shm_region_index);
	common->state = RPMSG_ETH_STATE_PROBE;

	ret = rpmsg_eth_get_shm_info(common);
	if (ret)
		return ret;

	/* Register the network device */
	ret = rpmsg_eth_init_ndev(common);
	if (ret)
		return ret;

	return 0;
}

static void rpmsg_eth_remove(struct rpmsg_device *rpdev)
{
	dev_dbg(&rpdev->dev, "rpmsg-eth client driver is removed\n");
}

static const struct rpmsg_eth_data ti_rpmsg_eth_data = {
	.shm_region_index = 2,
};

static struct rpmsg_device_id rpmsg_eth_id_table[] = {
	{ .name = "ti.shm-eth", .driver_data = (kernel_ulong_t)&ti_rpmsg_eth_data },
	{},
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_eth_id_table);

static struct rpmsg_driver rpmsg_eth_rpmsg_client = {
	.drv.name = KBUILD_MODNAME,
	.id_table = rpmsg_eth_id_table,
	.probe = rpmsg_eth_probe,
	.callback = rpmsg_eth_rpmsg_cb,
	.remove = rpmsg_eth_remove,
};
module_rpmsg_driver(rpmsg_eth_rpmsg_client);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MD Danish Anwar <danishanwar@ti.com>");
MODULE_DESCRIPTION("RPMsg Based Virtual Ethernet driver");
