/* SPDX-License-Identifier: GPL-2.0 */
/* Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#include "inter-core-virt-eth.h"

#define ICVE_MIN_PACKET_SIZE	ETH_ZLEN
#define ICVE_MAX_PACKET_SIZE	(ETH_FRAME_LEN + ETH_FCS_LEN)
#define ICVE_MAX_TX_QUEUES	1
#define ICVE_MAX_RX_QUEUES	1

#define TEST_DEBUG		1

#ifdef TEST_DEBUG
#define ICVE_MAX_BUFFERS	100 //TODO : Set to power of 2 to leverage shift operations
#endif

#define PKT_LEN_SIZE_TYPE	sizeof(u32)

/* 4 bytes to hold packet length and ICVE_MAX_PACKET_SIZE to hold packet */
#define ICVE_BUFFER_SIZE	(ICVE_MAX_PACKET_SIZE + PKT_LEN_SIZE_TYPE)

#define RX_POLL_TIMEOUT		250

#define icve_ndev_to_priv(ndev) \
	((struct icve_ndev_priv *)netdev_priv(ndev))
#define icve_ndev_to_port(ndev) (icve_ndev_to_priv(ndev)->port)
#define icve_ndev_to_common(ndev) (icve_ndev_to_port(ndev)->common)

static void icve_rx_timer(struct timer_list *timer)
{
	struct icve_port *port = from_timer(port, timer, rx_timer);
	struct napi_struct *napi;
	int num_pkts = 0;
	u32 head, tail;

	head = port->rx_buffer->head;
	tail = port->rx_buffer->tail;

	num_pkts = tail - head;
	num_pkts = num_pkts >= 0 ? num_pkts : (num_pkts + port->icve_max_buffers);

	napi = &port->rx_napi;
	if (num_pkts && likely(napi_schedule_prep(napi))) {
		__napi_schedule(napi);
	} else {
		mod_timer(&port->rx_timer, RX_POLL_TIMEOUT);
	}
}

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
	case ICVE_NOTIFY_MSG:
		rpmsg_type = msg->notify_msg.type;
		switch (rpmsg_type) {
		case ICVE_NOTIFY_REMOTE_READY:
			/* Turn on carrier once remote core signals ready */
			netif_carrier_on(port->ndev);
			break;
		case ICVE_NOTIFY_PORT_UP:
		case ICVE_NOTIFY_PORT_DOWN:
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
	case ICVE_NOTIFY_PORT_UP:
	case ICVE_NOTIFY_PORT_DOWN:
		msg->msg_hdr.msg_type = ICVE_NOTIFY_MSG;
		break;
	default:
		ret = -EINVAL;
		dev_err(common->dev, "Invalid RPMSG request\n");
	};

	return ret;
}

static int icve_rx_packets(struct napi_struct *napi, int budget)
{
	struct icve_port *port = container_of(napi, struct icve_port, rx_napi);
	u32 count, process_pkts;
	struct sk_buff *skb;
	u32 head, tail;
	u32 pkt_len;
	int num_pkts;

	head = port->rx_buffer->head;
	tail = port->rx_buffer->tail;

	num_pkts = tail - head;
	num_pkts = num_pkts >= 0 ? num_pkts : (num_pkts + port->icve_max_buffers);
	process_pkts = min(num_pkts, budget);
	count = 0;
	while (count < process_pkts) {
		memcpy((void *)&pkt_len,
		       (void *)port->rx_buffer->base_addr + ((head + count) * ICVE_BUFFER_SIZE),
		       PKT_LEN_SIZE_TYPE);

		/* Start building the skb */
		skb = napi_alloc_skb(napi, pkt_len);
		skb->dev = port->ndev;
		skb_put(skb, pkt_len);

		memcpy((void *)skb->data,
		       (void *)(port->rx_buffer->base_addr + PKT_LEN_SIZE_TYPE) + ((head + count) * ICVE_BUFFER_SIZE),
		       pkt_len);

		skb->protocol = eth_type_trans(skb, port->ndev);

		/* Push skb into network stack */
		napi_gro_receive(napi, skb);

		count++;
	}

	if (num_pkts) {
		port->rx_buffer->head = (port->rx_buffer->head + count) % port->icve_max_buffers;

		if (num_pkts < budget && napi_complete_done(napi, count))
			mod_timer(&port->rx_timer, RX_POLL_TIMEOUT);
	}
	return count;
}

#ifdef TEST_DEBUG
static int test_tx_rx_path(struct sk_buff *skb, struct net_device *ndev)
{
	struct icve_port *port = icve_ndev_to_port(ndev);
	u32 *data;
	u32 len;

	len = skb_headlen(skb);

	/* Copy length */
	memcpy((void *)port->rx_buffer->base_addr + (port->rx_buffer->tail * ICVE_BUFFER_SIZE),
	       (void *)&len, PKT_LEN_SIZE_TYPE);

	/* Copy data to shared mem */
	memcpy((void *)(port->rx_buffer->base_addr + PKT_LEN_SIZE_TYPE) + (port->rx_buffer->tail * ICVE_BUFFER_SIZE),
	       (void *)skb->data, len);

	data = (u32 *)(port->rx_buffer->base_addr + (port->rx_buffer->tail * ICVE_BUFFER_SIZE));

	port->rx_buffer->tail = (port->rx_buffer->tail + 1) % ICVE_MAX_BUFFERS;

	return 0;
}
#endif

static int icve_ndo_open(struct net_device *ndev)
{
	struct icve_common *common = icve_ndev_to_common(ndev);
	struct icve_port *port = icve_ndev_to_port(ndev);
	unsigned long flags;

	/* Send a msg to remote core signalling that we are ready */
	spin_lock_irqsave(&common->send_msg_lock, flags);
#ifndef TEST_DEBUG
	create_request(common, ICVE_NOTIFY_PORT_UP);
	rpmsg_send(common->rpdev->ept, (void *)(&common->send_msg), sizeof(common->send_msg));
#endif
	spin_unlock_irqrestore(&common->send_msg_lock, flags);

	if (!(port->tx_buffer && port->rx_buffer)) {
		netdev_err(ndev, "Shared memory not setup\n");
		return -EPERM;
	}

	netif_napi_add(ndev, &port->rx_napi, icve_rx_packets);
	napi_enable(&port->rx_napi);

	timer_setup(&port->rx_timer, icve_rx_timer, 0);
	mod_timer(&port->rx_timer, RX_POLL_TIMEOUT);

	return 0;
}

static int icve_ndo_stop(struct net_device *ndev)
{
	struct icve_common *common = icve_ndev_to_common(ndev);
	unsigned long flags;

	spin_lock_irqsave(&common->send_msg_lock, flags);
#ifndef TEST_DEBUG
	create_request(common, ICVE_NOTIFY_PORT_DOWN);
	rpmsg_send(common->rpdev->ept, (void *)(&common->send_msg), sizeof(common->send_msg));
#endif
	spin_unlock_irqrestore(&common->send_msg_lock, flags);
	return 0;
}

static netdev_tx_t icve_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct icve_port *port = icve_ndev_to_port(ndev);
	struct ethhdr *ether;
	u32 head, tail;
	u32 num_pkts;
	u32 len;

	ether = eth_hdr(skb);
	len = skb_headlen(skb);

	head = port->tx_buffer->head;
	tail = port->tx_buffer->tail;

	/* If the buffer queue is full, then drop packet */
	num_pkts = tail - head;
	num_pkts = num_pkts >= 0 ? num_pkts : (num_pkts + port->icve_max_buffers);
	if ((num_pkts + 1) == port->icve_max_buffers) {
		netdev_warn(ndev, "Tx buffer full\n");
		goto ring_full;
	}

	/* Copy length */
	memcpy((void *)port->tx_buffer->base_addr + (port->tx_buffer->tail * ICVE_BUFFER_SIZE),
	       (void *)&len, PKT_LEN_SIZE_TYPE);

	/* Copy data to shared mem */
	memcpy((void *)(port->tx_buffer->base_addr + PKT_LEN_SIZE_TYPE) +
	       (port->tx_buffer->tail * ICVE_BUFFER_SIZE),
	       (void *)skb->data, len);

#ifdef TEST_DEBUG
	/* For quick Rx path testing, inject Tx pkt back into network */
	test_tx_rx_path(skb, ndev);
#endif
	port->tx_buffer->tail = (port->tx_buffer->tail + 1) % port->icve_max_buffers;

	dev_consume_skb_any(skb);

	return NETDEV_TX_OK;

ring_full:
	return NETDEV_TX_BUSY;
}

static int icve_set_mac_address(struct net_device *ndev, void *addr)
{
	eth_mac_addr(ndev, addr);

	/* TODO : Inform remote core about MAC address change */
	return 0;
}

static const struct net_device_ops icve_netdev_ops = {
	.ndo_open		= icve_ndo_open,
	.ndo_stop		= icve_ndo_stop,
	.ndo_start_xmit		= icve_start_xmit,
	.ndo_set_mac_address	= icve_set_mac_address,
};

static int icve_init_ndev(struct icve_common *common)
{
	struct device *dev = &common->rpdev->dev;
	struct icve_ndev_priv *ndev_priv;
	struct icve_port *port;
	static u32 port_id;
	int err;

	port = common->port;
	port->common = common;
	port->port_id = port_id++;

	port->ndev = devm_alloc_etherdev_mqs(common->dev, sizeof(*ndev_priv),
					     ICVE_MAX_TX_QUEUES,
					     ICVE_MAX_RX_QUEUES);

	if (!port->ndev) {
		dev_err(dev, "error allocating net_device\n");
		return -ENOMEM;
	}

	ndev_priv = netdev_priv(port->ndev);
	ndev_priv->port = port;
	SET_NETDEV_DEV(port->ndev, dev);

	port->ndev->min_mtu = ICVE_MIN_PACKET_SIZE;
	port->ndev->max_mtu = ICVE_MAX_PACKET_SIZE;
	port->ndev->netdev_ops = &icve_netdev_ops;

#ifdef TEST_DEBUG
	/* Allocate memory to test without actual RPMsg handshaking */
	port->tx_buffer = devm_kzalloc(dev, sizeof(port->tx_buffer),
				       GFP_KERNEL);
	if (!port->tx_buffer) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}

	port->tx_buffer->base_addr = devm_kzalloc(dev, ICVE_BUFFER_SIZE * ICVE_MAX_BUFFERS,
						  GFP_KERNEL);
	if (!port->tx_buffer->base_addr) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}

	port->rx_buffer = devm_kzalloc(dev, sizeof(port->rx_buffer),
				       GFP_KERNEL);
	if (!port->rx_buffer) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	};

	port->rx_buffer->base_addr = devm_kzalloc(dev, ICVE_BUFFER_SIZE * ICVE_MAX_BUFFERS,
						  GFP_KERNEL);
	if (!port->rx_buffer->base_addr) {
		dev_err(dev, "Memory not available\n");
		return -ENOMEM;
	}

	port->icve_max_buffers = ICVE_MAX_BUFFERS;
#else
	/* Shared memory details will be sent by the remote core.
	 * So turn off the carrier, until both the virtual port and
	 * remote core is ready
	 */
	netif_carrier_off(port->ndev);

#endif
	err = register_netdev(port->ndev);

	if (err)
		dev_err(dev, "error registering icve net device %d\n", err);

	return 0;
}

static int icve_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct icve_common *common;
	unsigned long flags;
	int ret;

	common = devm_kzalloc(&rpdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	dev_set_drvdata(dev, common);

	common->port = devm_kzalloc(dev, sizeof(*common->port), GFP_KERNEL);
	common->dev = dev;
	common->rpdev = rpdev;

	spin_lock_init(&common->send_msg_lock);
	spin_lock_init(&common->recv_msg_lock);

	/* Register the network device */
	ret = icve_init_ndev(common);
	if (ret)
		return ret;

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
