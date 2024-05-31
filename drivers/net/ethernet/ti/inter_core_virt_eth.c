// SPDX-License-Identifier: GPL-2.0
/* Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#include "inter_core_virt_eth.h"

#define ICVE_MIN_PACKET_SIZE ETH_ZLEN
#define ICVE_MAX_PACKET_SIZE 1540 //(ETH_FRAME_LEN + ETH_FCS_LEN)
#define ICVE_MAX_TX_QUEUES 1
#define ICVE_MAX_RX_QUEUES 1

#define PKT_LEN_SIZE_TYPE sizeof(u32)
#define MAGIC_NUM_SIZE_TYPE sizeof(u32)

/* 4 bytes to hold packet length and ICVE_MAX_PACKET_SIZE to hold packet */
#define ICVE_BUFFER_SIZE \
	(ICVE_MAX_PACKET_SIZE + PKT_LEN_SIZE_TYPE + MAGIC_NUM_SIZE_TYPE)

#define RX_POLL_TIMEOUT 1000 /* 1000usec */
#define RX_POLL_JIFFIES (jiffies + usecs_to_jiffies(RX_POLL_TIMEOUT))

#define STATE_MACHINE_TIME msecs_to_jiffies(100)
#define ICVE_REQ_TIMEOUT msecs_to_jiffies(100)

#define icve_ndev_to_priv(ndev) ((struct icve_ndev_priv *)netdev_priv(ndev))
#define icve_ndev_to_port(ndev) (icve_ndev_to_priv(ndev)->port)
#define icve_ndev_to_common(ndev) (icve_ndev_to_port(ndev)->common)

static int create_request(struct icve_common *common,
			  enum icve_rpmsg_type rpmsg_type)
{
	struct message *msg = &common->send_msg;
	int ret = 0;

	msg->msg_hdr.src_id = common->port->port_id;
	msg->req_msg.type = rpmsg_type;

	switch (rpmsg_type) {
	case ICVE_REQ_SHM_INFO:
		msg->msg_hdr.msg_type = ICVE_REQUEST_MSG;
		break;
	case ICVE_REQ_SET_MAC_ADDR:
		msg->msg_hdr.msg_type = ICVE_REQUEST_MSG;
		ether_addr_copy(msg->req_msg.mac_addr.addr,
				common->port->ndev->dev_addr);
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

static int icve_create_send_request(struct icve_common *common,
				    enum icve_rpmsg_type rpmsg_type,
				    bool wait)
{
	unsigned long flags;
	int ret;

	if (wait)
		reinit_completion(&common->sync_msg);

	spin_lock_irqsave(&common->send_msg_lock, flags);
	create_request(common, rpmsg_type);
	rpmsg_send(common->rpdev->ept, (void *)(&common->send_msg),
		   sizeof(common->send_msg));
	spin_unlock_irqrestore(&common->send_msg_lock, flags);

	if (wait) {
		ret = wait_for_completion_timeout(&common->sync_msg,
						  ICVE_REQ_TIMEOUT);

		if (!ret) {
			dev_err(common->dev, "Failed to receive response within %ld jiffies\n",
				ICVE_REQ_TIMEOUT);
			ret = -ETIMEDOUT;
			return ret;
		}
	}
	return ret;
}

static void icve_state_machine(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct icve_common *common;
	struct icve_port *port;

	common = container_of(dwork, struct icve_common, state_work);
	port = common->port;

	mutex_lock(&common->state_lock);

	switch (common->state) {
	case ICVE_STATE_PROBE:
		break;
	case ICVE_STATE_OPEN:
		icve_create_send_request(common, ICVE_REQ_SHM_INFO, false);
		break;
	case ICVE_STATE_CLOSE:
		break;
	case ICVE_STATE_READY:
		icve_create_send_request(common, ICVE_REQ_SET_MAC_ADDR, false);
		napi_enable(&port->rx_napi);
		netif_carrier_on(port->ndev);
		mod_timer(&port->rx_timer, RX_POLL_TIMEOUT);
		break;
	case ICVE_STATE_RUNNING:
		break;
	}
	mutex_unlock(&common->state_lock);
}

static void icve_rx_timer(struct timer_list *timer)
{
	struct icve_port *port = from_timer(port, timer, rx_timer);
	struct napi_struct *napi;
	int num_pkts = 0;
	u32 head, tail;

	head = port->rx_buffer->head->index;
	tail = port->rx_buffer->tail->index;

	num_pkts = tail - head;
	num_pkts = num_pkts >= 0 ? num_pkts :
				   (num_pkts + port->icve_rx_max_buffers);

	napi = &port->rx_napi;
	if (num_pkts && likely(napi_schedule_prep(napi)))
		__napi_schedule(napi);
	else
		mod_timer(&port->rx_timer, RX_POLL_JIFFIES);
}

static int icve_rpmsg_cb(struct rpmsg_device *rpdev, void *data, int len,
			 void *priv, u32 src)
{
	struct icve_common *common = dev_get_drvdata(&rpdev->dev);
	struct message *msg = (struct message *)data;
	struct icve_port *port = common->port;
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
		switch (rpmsg_type) {
		case ICVE_RESP_SHM_INFO:
			/* Retrieve Tx and Rx shared memory info from msg */
			port->tx_buffer->head =
				ioremap(msg->resp_msg.shm_info.shm_info_tx.base_addr,
					sizeof(*port->tx_buffer->head));

			port->tx_buffer->buf->base_addr =
				ioremap((msg->resp_msg.shm_info.shm_info_tx.base_addr +
					sizeof(*port->tx_buffer->head)),
					(msg->resp_msg.shm_info.shm_info_tx.num_pkt_bufs *
					 msg->resp_msg.shm_info.shm_info_tx.buff_slot_size));

			port->tx_buffer->tail =
				ioremap(msg->resp_msg.shm_info.shm_info_tx.base_addr +
					sizeof(*port->tx_buffer->head) +
					(msg->resp_msg.shm_info.shm_info_tx.num_pkt_bufs *
					msg->resp_msg.shm_info.shm_info_tx.buff_slot_size),
					sizeof(*port->tx_buffer->tail));

			port->icve_tx_max_buffers = msg->resp_msg.shm_info.shm_info_tx.num_pkt_bufs;

			port->rx_buffer->head =
				ioremap(msg->resp_msg.shm_info.shm_info_rx.base_addr,
					sizeof(*port->rx_buffer->head));

			port->rx_buffer->buf->base_addr =
				ioremap(msg->resp_msg.shm_info.shm_info_rx.base_addr +
					sizeof(*port->rx_buffer->head),
					(msg->resp_msg.shm_info.shm_info_rx.num_pkt_bufs *
					 msg->resp_msg.shm_info.shm_info_rx.buff_slot_size));

			port->rx_buffer->tail =
				ioremap(msg->resp_msg.shm_info.shm_info_rx.base_addr +
					sizeof(*port->rx_buffer->head) +
					(msg->resp_msg.shm_info.shm_info_rx.num_pkt_bufs *
					msg->resp_msg.shm_info.shm_info_rx.buff_slot_size),
					sizeof(*port->rx_buffer->tail));

			port->icve_rx_max_buffers =
				msg->resp_msg.shm_info.shm_info_rx.num_pkt_bufs;

			mutex_lock(&common->state_lock);
			common->state = ICVE_STATE_READY;
			mutex_unlock(&common->state_lock);

			mod_delayed_work(system_wq,
					 &common->state_work,
					 STATE_MACHINE_TIME);

			break;
		case ICVE_RESP_SET_MAC_ADDR:
			break;
		}

		break;

	case ICVE_NOTIFY_MSG:
		rpmsg_type = msg->notify_msg.type;
		switch (rpmsg_type) {
		case ICVE_NOTIFY_REMOTE_READY:
			mutex_lock(&common->state_lock);
			common->state = ICVE_STATE_RUNNING;
			mutex_unlock(&common->state_lock);

			mod_delayed_work(system_wq,
					 &common->state_work,
					 STATE_MACHINE_TIME);
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

static int icve_rx_packets(struct napi_struct *napi, int budget)
{
	struct icve_port *port = container_of(napi, struct icve_port, rx_napi);
	u32 count, process_pkts;
	struct sk_buff *skb;
	u32 head, tail;
	int num_pkts;
	u32 pkt_len;

	head = port->rx_buffer->head->index;
	tail = port->rx_buffer->tail->index;

	num_pkts = head - tail;

	num_pkts = num_pkts >= 0 ? num_pkts :
				   (num_pkts + port->icve_rx_max_buffers);
	process_pkts = min(num_pkts, budget);
	count = 0;
	while (count < process_pkts) {
		memcpy_fromio((void *)&pkt_len,
			      (void *)(port->rx_buffer->buf->base_addr +
			      MAGIC_NUM_SIZE_TYPE +
			      (((tail + count) % port->icve_rx_max_buffers) *
			      ICVE_BUFFER_SIZE)),
			      PKT_LEN_SIZE_TYPE);
		/* Start building the skb */
		skb = napi_alloc_skb(napi, pkt_len);
		if (!skb) {
			port->ndev->stats.rx_dropped++;
			goto rx_dropped;
		}

		skb->dev = port->ndev;
		skb_put(skb, pkt_len);
		memcpy_fromio((void *)skb->data,
			      (void *)(port->rx_buffer->buf->base_addr +
			      PKT_LEN_SIZE_TYPE + MAGIC_NUM_SIZE_TYPE +
			      (((tail + count) % port->icve_rx_max_buffers) *
			      ICVE_BUFFER_SIZE)),
			      pkt_len);

		skb->protocol = eth_type_trans(skb, port->ndev);

		/* Push skb into network stack */
		napi_gro_receive(napi, skb);

		count++;
		port->ndev->stats.rx_packets++;
		port->ndev->stats.rx_bytes += skb->len;
	}

rx_dropped:

	if (num_pkts) {
		port->rx_buffer->tail->index =
			(port->rx_buffer->tail->index + count) %
			port->icve_rx_max_buffers;

		if (num_pkts < budget && napi_complete_done(napi, count))
			mod_timer(&port->rx_timer, RX_POLL_TIMEOUT);
	}

	return count;
}

static int icve_ndo_open(struct net_device *ndev)
{
	struct icve_common *common = icve_ndev_to_common(ndev);

	mutex_lock(&common->state_lock);
	common->state = ICVE_STATE_OPEN;
	mutex_unlock(&common->state_lock);
	mod_delayed_work(system_wq, &common->state_work, msecs_to_jiffies(100));

	return 0;
}

static int icve_ndo_stop(struct net_device *ndev)
{
	struct icve_common *common = icve_ndev_to_common(ndev);
	struct icve_port *port = icve_ndev_to_port(ndev);

	mutex_lock(&common->state_lock);
	common->state = ICVE_STATE_CLOSE;
	mutex_unlock(&common->state_lock);

	netif_carrier_off(port->ndev);

	__dev_mc_unsync(ndev, icve_del_mc_addr);
	__hw_addr_init(&common->mc_list);

	cancel_delayed_work_sync(&common->state_work);
	del_timer_sync(&port->rx_timer);
	napi_disable(&port->rx_napi);

	cancel_work_sync(&common->rx_mode_work);

	return 0;
}

static netdev_tx_t icve_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct icve_port *port = icve_ndev_to_port(ndev);
	u32 head, tail;
	int num_pkts;
	u32 len;

	len = skb_headlen(skb);
	head = port->tx_buffer->head->index;
	tail = port->tx_buffer->tail->index;

	/* If the buffer queue is full, then drop packet */
	num_pkts = head - tail;
	num_pkts = num_pkts >= 0 ? num_pkts :
				   (num_pkts + port->icve_tx_max_buffers);

	if ((num_pkts + 1) == port->icve_tx_max_buffers) {
		netdev_warn(ndev, "Tx buffer full %d\n", num_pkts);
		goto ring_full;
	}
	/* Copy length */
	memcpy_toio((void *)port->tx_buffer->buf->base_addr +
			    MAGIC_NUM_SIZE_TYPE +
			    (port->tx_buffer->head->index * ICVE_BUFFER_SIZE),
		    (void *)&len, PKT_LEN_SIZE_TYPE);
	/* Copy data to shared mem */
	memcpy_toio((void *)(port->tx_buffer->buf->base_addr +
			     MAGIC_NUM_SIZE_TYPE + PKT_LEN_SIZE_TYPE +
			     (port->tx_buffer->head->index * ICVE_BUFFER_SIZE)),
		    (void *)skb->data, len);
	port->tx_buffer->head->index =
		(port->tx_buffer->head->index + 1) % port->icve_tx_max_buffers;

	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;

	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

ring_full:
	return NETDEV_TX_BUSY;
}

static int icve_set_mac_address(struct net_device *ndev, void *addr)
{
	struct icve_common *common = icve_ndev_to_common(ndev);
	int ret;

	ret = eth_mac_addr(ndev, addr);

	if (ret < 0)
		return ret;
	icve_create_send_request(common, ICVE_REQ_SET_MAC_ADDR, false);
	return ret;
}

static const struct net_device_ops icve_netdev_ops = {
	.ndo_open = icve_ndo_open,
	.ndo_stop = icve_ndo_stop,
	.ndo_start_xmit = icve_start_xmit,
	.ndo_set_mac_address = icve_set_mac_address,
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
	};
	netif_carrier_off(port->ndev);

	netif_napi_add(port->ndev, &port->rx_napi, icve_rx_packets);
	timer_setup(&port->rx_timer, icve_rx_timer, 0);
	err = register_netdev(port->ndev);

	if (err)
		dev_err(dev, "error registering icve net device %d\n", err);
	return 0;
}

static int icve_rpmsg_probe(struct rpmsg_device *rpdev)
{
	struct device *dev = &rpdev->dev;
	struct icve_common *common;
	int ret = 0;

	common = devm_kzalloc(&rpdev->dev, sizeof(*common), GFP_KERNEL);
	if (!common)
		return -ENOMEM;

	dev_set_drvdata(dev, common);

	common->port = devm_kzalloc(dev, sizeof(*common->port), GFP_KERNEL);
	common->dev = dev;
	common->rpdev = rpdev;
	common->state = ICVE_STATE_PROBE;
	spin_lock_init(&common->send_msg_lock);
	spin_lock_init(&common->recv_msg_lock);
	mutex_init(&common->state_lock);
	INIT_DELAYED_WORK(&common->state_work, icve_state_machine);
	init_completion(&common->sync_msg);

	/* Register the network device */
	ret = icve_init_ndev(common);
	if (ret)
		return ret;
	return 0;
}

static void icve_rpmsg_remove(struct rpmsg_device *rpdev)
{
	struct icve_common *common = dev_get_drvdata(&rpdev->dev);
	struct icve_port *port = common->port;

	netif_napi_del(&port->rx_napi);
	del_timer_sync(&port->rx_timer);
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
