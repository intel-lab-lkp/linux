// SPDX-License-Identifier: GPL-2.0
/* RPMsg Based Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#include <linux/of.h>
#include "rpmsg_eth.h"

/**
 * rpmsg_eth_validate_handshake - Validate handshake parameters from remote
 * @port: Pointer to rpmsg_eth_port structure
 * @shm_info: Pointer to shared memory info received from remote
 *
 * Checks the magic numbers, base address, and TX/RX offsets in the handshake
 * response to ensure they match expected values and are within valid ranges.
 *
 * Return: 0 on success, -EINVAL on validation failure.
 */
static int rpmsg_eth_validate_handshake(struct rpmsg_eth_port *port,
					struct rpmsg_eth_shm *shm_info)
{
	if (port->tx_buffer->head->magic_num != RPMSG_ETH_SHM_MAGIC_NUM ||
	    port->tx_buffer->tail->magic_num != RPMSG_ETH_SHM_MAGIC_NUM ||
	    port->rx_buffer->head->magic_num != RPMSG_ETH_SHM_MAGIC_NUM ||
	    port->rx_buffer->tail->magic_num != RPMSG_ETH_SHM_MAGIC_NUM) {
		dev_err(port->common->dev, "Magic number mismatch in handshake: tx_head=0x%x, tx_tail=0x%x, rx_head=0x%x, rx_tail=0x%x\n",
			port->tx_buffer->head->magic_num,
			port->tx_buffer->tail->magic_num,
			port->rx_buffer->head->magic_num,
			port->rx_buffer->tail->magic_num);
		return -EINVAL;
	}

	if (shm_info->base_addr != port->buf_start_addr) {
		dev_err(port->common->dev, "Base address mismatch in handshake: expected=0x%x, received=0x%x\n",
			port->buf_start_addr,
			shm_info->base_addr);
		return -EINVAL;
	}

	if (shm_info->tx_offset >= port->buf_size ||
	    shm_info->rx_offset >= port->buf_size) {
		dev_err(port->common->dev, "TX/RX offset out of range in handshake: tx_offset=0x%x, rx_offset=0x%x, size=0x%x\n",
			shm_info->tx_offset,
			shm_info->rx_offset,
			port->buf_size);
		return -EINVAL;
	}

	return 0;
}

static void rpmsg_eth_map_buffers(struct rpmsg_eth_port *port,
				  struct message *msg)
{
	port->tx_buffer->head =
		(struct rpmsg_eth_shm_index __force *)
		 (ioremap(msg->resp_msg.shm_info.base_addr +
			  msg->resp_msg.shm_info.tx_offset,
			  sizeof(*port->tx_buffer->head)));

	port->tx_buffer->buf->base_addr =
		ioremap((msg->resp_msg.shm_info.base_addr +
			 msg->resp_msg.shm_info.tx_offset +
			 sizeof(*port->tx_buffer->head)),
			 (msg->resp_msg.shm_info.num_pkt_bufs *
			  msg->resp_msg.shm_info.buff_slot_size));

	port->tx_buffer->tail =
		(struct rpmsg_eth_shm_index __force *)
		 (ioremap(msg->resp_msg.shm_info.base_addr +
			  msg->resp_msg.shm_info.tx_offset +
			  sizeof(*port->tx_buffer->head) +
			  (msg->resp_msg.shm_info.num_pkt_bufs *
			   msg->resp_msg.shm_info.buff_slot_size),
			  sizeof(*port->tx_buffer->tail)));

	port->rx_buffer->head =
		(struct rpmsg_eth_shm_index __force *)
		 (ioremap(msg->resp_msg.shm_info.base_addr +
			  msg->resp_msg.shm_info.rx_offset,
			  sizeof(*port->rx_buffer->head)));

	port->rx_buffer->buf->base_addr =
		ioremap(msg->resp_msg.shm_info.base_addr +
			msg->resp_msg.shm_info.rx_offset +
			sizeof(*port->rx_buffer->head),
			(msg->resp_msg.shm_info.num_pkt_bufs *
			 msg->resp_msg.shm_info.buff_slot_size));

	port->rx_buffer->tail =
		(struct rpmsg_eth_shm_index __force *)
		 (ioremap(msg->resp_msg.shm_info.base_addr +
			  msg->resp_msg.shm_info.rx_offset +
			  sizeof(*port->rx_buffer->head) +
			  (msg->resp_msg.shm_info.num_pkt_bufs *
			   msg->resp_msg.shm_info.buff_slot_size),
			  sizeof(*port->rx_buffer->tail)));
}

static void rpmsg_eth_unmap_buffers(struct rpmsg_eth_port *port)
{
	if (port->tx_buffer && port->tx_buffer->head) {
		iounmap((void __iomem *)port->tx_buffer->head);
		port->tx_buffer->head = NULL;
	}
	if (port->tx_buffer && port->tx_buffer->buf &&
	    port->tx_buffer->buf->base_addr) {
		iounmap((void __iomem *)port->tx_buffer->buf->base_addr);
		port->tx_buffer->buf->base_addr = NULL;
	}
	if (port->tx_buffer && port->tx_buffer->tail) {
		iounmap((void __iomem *)port->tx_buffer->tail);
		port->tx_buffer->tail = NULL;
	}

	if (port->rx_buffer && port->rx_buffer->head) {
		iounmap((void __iomem *)port->rx_buffer->head);
		port->rx_buffer->head = NULL;
	}
	if (port->rx_buffer && port->rx_buffer->buf &&
	    port->rx_buffer->buf->base_addr) {
		iounmap((void __iomem *)port->rx_buffer->buf->base_addr);
		port->rx_buffer->buf->base_addr = NULL;
	}
	if (port->rx_buffer && port->rx_buffer->tail) {
		iounmap((void __iomem *)port->rx_buffer->tail);
		port->rx_buffer->tail = NULL;
	}
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
			rpmsg_eth_map_buffers(port, msg);

			port->rpmsg_eth_tx_max_buffers =
				msg->resp_msg.shm_info.num_pkt_bufs;
			port->rpmsg_eth_rx_max_buffers =
				msg->resp_msg.shm_info.num_pkt_bufs;

			/* Handshake validation */
			ret = rpmsg_eth_validate_handshake(port, &msg->resp_msg.shm_info);
			if (ret) {
				dev_err(common->dev, "RPMSG handshake failed %d\n", ret);
				rpmsg_eth_unmap_buffers(port);
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

static int rpmsg_eth_init_ndev(struct rpmsg_eth_common *common)
{
	struct device *dev = &common->rpdev->dev;
	struct rpmsg_eth_ndev_priv *ndev_priv;
	struct rpmsg_eth_port *port;
	static u32 port_id;
	int err;

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

	/* Allocate memory to test without actual RPMsg handshaking */
	port->tx_buffer =
		devm_kzalloc(dev, sizeof(*port->tx_buffer), GFP_KERNEL);
	if (!port->tx_buffer) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}

	port->tx_buffer->buf =
		devm_kzalloc(dev, sizeof(*port->tx_buffer->buf), GFP_KERNEL);
	if (!port->tx_buffer->buf) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}

	port->rx_buffer =
		devm_kzalloc(dev, sizeof(*port->rx_buffer), GFP_KERNEL);
	if (!port->rx_buffer) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}

	port->rx_buffer->buf =
		devm_kzalloc(dev, sizeof(*port->rx_buffer->buf), GFP_KERNEL);
	if (!port->rx_buffer->buf) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}
	netif_carrier_off(port->ndev);

	err = register_netdev(port->ndev);

	if (err)
		dev_err(dev, "error registering rpmsg_eth net device %d\n", err);
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

static void rpmsg_eth_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct rpmsg_eth_common *common = dev_get_drvdata(&rpdev->dev);
	struct rpmsg_eth_port *port = common->port;

	/* Unmap ioremap'd regions */
	rpmsg_eth_unmap_buffers(port);
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
